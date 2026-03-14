#include "memrpc/server/rpc_server.h"

#include <algorithm>
#include <array>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memrpc/core/protocol.h"
#include "memrpc/core/runtime_utils.h"
#include "memrpc/core/task_executor.h"
#include "memrpc/server/handler.h"
#include "core/session.h"
#include "virus_protection_service_log.h"

namespace MemRpc {

namespace {

constexpr auto RESPONSE_RETRY_BUDGET = std::chrono::milliseconds(50);
constexpr auto EVENT_RETRY_BUDGET = std::chrono::milliseconds(10);

class ThreadPoolExecutor final : public TaskExecutor {
 public:
  explicit ThreadPoolExecutor(uint32_t threadCount) : running_(true) {
    const uint32_t threads = std::max(1U, threadCount);
    queueCapacity_ = threads;
    for (uint32_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  ~ThreadPoolExecutor() override {
    Stop();
  }

  bool TrySubmit(std::function<void()> task) override {
    if (!task) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || queue_.size() >= queueCapacity_) {
        return false;
      }
      queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  bool HasCapacity() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && queue_.size() < queueCapacity_;
  }

  bool WaitForCapacity(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
      return !running_ || queue_.size() < queueCapacity_;
    });
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      running_ = false;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

 private:
  void WorkerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
      }
      task();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool running_ = false;
  uint32_t queueCapacity_ = 1;
};

bool IsHighPriority(const RequestRingEntry& entry) {
  return entry.priority == static_cast<uint8_t>(Priority::High);
}

}  // namespace

struct RpcServer::Impl {
  struct CompletionState {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    StatusCode status = StatusCode::EngineInternalError;
  };

  struct CompletionItem {
    ResponseRingEntry entry;
    std::chrono::milliseconds retryBudget{0};
    bool breakSessionOnFailure = false;
    std::shared_ptr<CompletionState> completion;
  };

  BootstrapHandles handles{};
  ServerOptions options{};
  Session session;
  std::shared_ptr<TaskExecutor> highExecutor;
  std::shared_ptr<TaskExecutor> normalExecutor;
  std::mutex completionMutex;
  std::condition_variable completionCv;
  std::queue<CompletionItem> completionQueue;
  uint32_t completionQueueCapacity = 0;
  uint32_t pendingCompletionCount = 0;
  std::thread responseWriterThread;
  std::atomic<bool> responseWriterRunning{false};
  std::atomic<bool> responseWriterWaitingForCredit{false};
  std::thread dispatcherThread;
  std::atomic<bool> running{false};
  std::unordered_map<uint16_t, RpcHandler> handlers;

  PollEventFdResult WaitForResponseCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.Handles().respCreditEventFd;
    if (fd < 0) {
      return PollEventFdResult::Failed;
    }
    pollfd pollFd{fd, POLLIN, 0};
    responseWriterWaitingForCredit.store(true);
    [[maybe_unused]] const auto clearWaiting =
        MakeScopeExit([this] { responseWriterWaitingForCredit.store(false); });
    while (responseWriterRunning.load(std::memory_order_acquire)) {
      const int64_t remainingMs = RemainingTimeoutMs(deadline);
      if (remainingMs <= 0) {
        return PollEventFdResult::Timeout;
      }
      const auto waitResult = PollEventFd(&pollFd, static_cast<int>(remainingMs));
      if (waitResult == PollEventFdResult::Retry) {
        continue;
      }
      return waitResult;
    }
    return PollEventFdResult::Failed;
  }

  StatusCode PushResponseWithRetry(const ResponseRingEntry& response,
                                   std::chrono::milliseconds retryBudget) {
    const auto deadline = std::chrono::steady_clock::now() + retryBudget;
    while (true) {
      const StatusCode status = session.PushResponse(response);
      if (status == StatusCode::Ok || status != StatusCode::QueueFull) {
        return status;
      }
      if (DeadlineReached(deadline)) {
        return StatusCode::QueueFull;
      }
      const auto waitResult = WaitForResponseCredit(deadline);
      if (waitResult == PollEventFdResult::Ready) {
        continue;
      }
      return waitResult == PollEventFdResult::Timeout ? StatusCode::QueueFull
                                                      : StatusCode::PeerDisconnected;
    }
  }

  void MarkSessionBroken() {
    session.SetState(Session::SessionState::Broken);
    if (session.Handles().respEventFd >= 0) {
      (void)SignalEventFd(session.Handles().respEventFd);
    }
  }

  static void CompleteItem(const std::shared_ptr<CompletionState>& completion, StatusCode status) {
    if (completion == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->status = status;
    completion->ready = true;
    completion->cv.notify_one();
  }

  void OnCompletionItemFinished() {
    std::lock_guard<std::mutex> lock(completionMutex);
    if (pendingCompletionCount > 0) {
      --pendingCompletionCount;
    }
  }

  bool EnqueueCompletion(CompletionItem item) {
    std::lock_guard<std::mutex> lock(completionMutex);
    if (!responseWriterRunning.load(std::memory_order_relaxed) ||
        pendingCompletionCount >= completionQueueCapacity) {
      return false;
    }
    completionQueue.push(std::move(item));
    ++pendingCompletionCount;
    completionCv.notify_one();
    return true;
  }

  bool WaitAndPopCompletionItem(CompletionItem* item) {
    if (item == nullptr) {
      return false;
    }
    std::unique_lock<std::mutex> lock(completionMutex);
    completionCv.wait(lock, [this] {
      return !responseWriterRunning.load(std::memory_order_relaxed) || !completionQueue.empty();
    });
    if (!responseWriterRunning.load(std::memory_order_relaxed) && completionQueue.empty()) {
      return false;
    }
    *item = std::move(completionQueue.front());
    completionQueue.pop();
    return true;
  }

  void FailPendingCompletionItems(StatusCode status) {
    std::queue<CompletionItem> queued;
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      queued.swap(completionQueue);
      pendingCompletionCount = 0;
    }
    while (!queued.empty()) {
      CompleteItem(queued.front().completion, status);
      queued.pop();
    }
  }

  void StopResponseWriter() {
    responseWriterRunning.store(false);
    if (session.Handles().respCreditEventFd >= 0) {
      (void)SignalEventFd(session.Handles().respCreditEventFd);
    }
    completionCv.notify_all();
    if (responseWriterThread.joinable()) {
      responseWriterThread.join();
    }
    FailPendingCompletionItems(StatusCode::PeerDisconnected);
  }

  void ResponseWriterLoop() {
    CompletionItem item;
    while (WaitAndPopCompletionItem(&item)) {
      const StatusCode status = PushResponseWithRetry(item.entry, item.retryBudget);
      if (status == StatusCode::Ok && !SignalEventFd(session.Handles().respEventFd)) {
        HILOGW("response published without wakeup signal, request_id=%{public}llu",
               static_cast<unsigned long long>(item.entry.requestId));
      }
      if (status != StatusCode::Ok && item.breakSessionOnFailure) {
        MarkSessionBroken();
      }
      CompleteItem(item.completion, status);
      OnCompletionItemFinished();
    }
  }

  RpcServerCall BuildServerCall(const RequestRingEntry& requestEntry) const {
    RpcServerCall call;
    call.opcode = requestEntry.opcode;
    call.priority = IsHighPriority(requestEntry) ? Priority::High : Priority::Normal;
    call.queueTimeoutMs = requestEntry.queueTimeoutMs;
    call.execTimeoutMs = requestEntry.execTimeoutMs;
    call.flags = requestEntry.flags;
    call.payload = PayloadView(requestEntry.payload.data(), requestEntry.payloadSize);
    return call;
  }

  RpcServerReply InvokeHandlerWithTimeout(const RequestRingEntry& requestEntry) {
    RpcServerReply reply;
    const auto it = handlers.find(requestEntry.opcode);
    if (it == handlers.end()) {
      reply.status = StatusCode::InvalidArgument;
      return reply;
    }

    RpcServerCall call = BuildServerCall(requestEntry);
    const auto start = std::chrono::steady_clock::now();
    it->second(call, &reply);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    if (requestEntry.execTimeoutMs > 0 &&
        elapsedMs > static_cast<long long>(requestEntry.execTimeoutMs)) {
      reply.status = StatusCode::ExecTimeout;
      reply.payload.clear();
    }
    return reply;
  }

  StatusCode WriteResponse(const RequestRingEntry& requestEntry, RpcServerReply reply) {
    if (!session.Valid() || session.Header() == nullptr) {
      return StatusCode::PeerDisconnected;
    }

    if (reply.payload.size() > session.Header()->maxResponseBytes ||
        reply.payload.size() > ResponseRingEntry::INLINE_PAYLOAD_BYTES) {
      reply.status = StatusCode::PayloadTooLarge;
      reply.payload.clear();
    }

    ResponseRingEntry entry;
    entry.requestId = requestEntry.requestId;
    entry.messageKind = ResponseMessageKind::Reply;
    entry.statusCode = static_cast<uint32_t>(reply.status);
    entry.errorCode = reply.errorCode;
    entry.resultSize = static_cast<uint32_t>(reply.payload.size());
    if (!reply.payload.empty()) {
      std::memcpy(entry.payload.data(), reply.payload.data(), reply.payload.size());
    }

    auto completion = std::make_shared<CompletionState>();
    CompletionItem item;
    item.entry = entry;
    item.retryBudget = RESPONSE_RETRY_BUDGET;
    item.breakSessionOnFailure = true;
    item.completion = completion;
    if (!EnqueueCompletion(std::move(item))) {
      MarkSessionBroken();
      return StatusCode::PeerDisconnected;
    }

    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion] { return completion->ready; });
    return completion->status;
  }

  StatusCode PublishEvent(const RpcEvent& event) {
    if (!session.Valid() || session.Handles().respEventFd < 0 || session.Header() == nullptr) {
      HILOGE("PublishEvent failed, session is not ready");
      return StatusCode::EngineInternalError;
    }
    if (event.payload.size() > session.Header()->maxResponseBytes ||
        event.payload.size() > ResponseRingEntry::INLINE_PAYLOAD_BYTES) {
      return StatusCode::PayloadTooLarge;
    }

    ResponseRingEntry entry;
    entry.messageKind = ResponseMessageKind::Event;
    entry.eventDomain = event.eventDomain;
    entry.eventType = event.eventType;
    entry.flags = event.flags;
    entry.resultSize = static_cast<uint32_t>(event.payload.size());
    if (!event.payload.empty()) {
      std::memcpy(entry.payload.data(), event.payload.data(), event.payload.size());
    }

    auto completion = std::make_shared<CompletionState>();
    CompletionItem item;
    item.entry = entry;
    item.retryBudget = EVENT_RETRY_BUDGET;
    item.completion = completion;
    if (!EnqueueCompletion(std::move(item))) {
      return StatusCode::QueueFull;
    }

    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion] { return completion->ready; });
    return completion->status;
  }

  void ProcessEntry(const RequestRingEntry& requestEntry) {
    if (!session.Valid() || session.Header() == nullptr) {
      return;
    }
    if (requestEntry.payloadSize > session.Header()->maxRequestBytes ||
        requestEntry.payloadSize > RequestRingEntry::INLINE_PAYLOAD_BYTES) {
      RpcServerReply reply;
      reply.status = StatusCode::PayloadTooLarge;
      (void)WriteResponse(requestEntry, std::move(reply));
      return;
    }

    const uint32_t nowMs = MonotonicNowMs();
    if (requestEntry.queueTimeoutMs > 0 &&
        nowMs - requestEntry.enqueueMonoMs > requestEntry.queueTimeoutMs) {
      RpcServerReply reply;
      reply.status = StatusCode::QueueTimeout;
      (void)WriteResponse(requestEntry, std::move(reply));
      return;
    }

    RpcServerReply reply = InvokeHandlerWithTimeout(requestEntry);
    (void)WriteResponse(requestEntry, std::move(reply));
  }

  bool DrainQueue(QueueKind kind, TaskExecutor* executor) {
    if (executor == nullptr || session.Header() == nullptr) {
      return false;
    }

    bool drained = false;
    RequestRingEntry entry;
    bool ringBecameNotFull = false;
    while (executor->HasCapacity() && session.PopRequest(kind, &entry)) {
      const RingCursor& cursor =
          kind == QueueKind::HighRequest ? session.Header()->highRing : session.Header()->normalRing;
      if (cursor.capacity != 0 && RingCount(cursor) + 1U == cursor.capacity) {
        ringBecameNotFull = true;
      }
      drained = true;
      const RequestRingEntry captured = entry;
      (void)executor->TrySubmit([this, captured] { ProcessEntry(captured); });
    }
    if (ringBecameNotFull) {
      (void)SignalEventFd(session.Handles().reqCreditEventFd);
    }
    return drained;
  }

  bool HandleBackloggedQueues() {
    if (session.Header() == nullptr || highExecutor == nullptr || normalExecutor == nullptr) {
      return false;
    }
    const bool highBacklogged =
        RingCount(session.Header()->highRing) > 0 && !highExecutor->HasCapacity();
    const bool normalBacklogged =
        RingCount(session.Header()->normalRing) > 0 && !normalExecutor->HasCapacity();
    if (highBacklogged) {
      (void)highExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    if (normalBacklogged) {
      (void)normalExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    return false;
  }

  bool SpinForRingActivity(int iterations) const {
    if (session.Header() == nullptr) {
      return false;
    }
    for (int i = 0; i < iterations; ++i) {
      if (RingCount(session.Header()->highRing) > 0 ||
          RingCount(session.Header()->normalRing) > 0) {
        return true;
      }
      CpuRelax();
    }
    return false;
  }

  void DispatcherLoop() {
    constexpr int SPIN_ITERATIONS = 256;
    std::array<pollfd, 2> fds{{
        {session.Handles().highReqEventFd, POLLIN, 0},
        {session.Handles().normalReqEventFd, POLLIN, 0},
    }};

    while (running.load(std::memory_order_acquire)) {
      bool highWork = DrainQueue(QueueKind::HighRequest, highExecutor.get());
      if (!highWork) {
        DrainQueue(QueueKind::NormalRequest, normalExecutor.get());
      }

      if (HandleBackloggedQueues()) {
        continue;
      }
      if (SpinForRingActivity(SPIN_ITERATIONS)) {
        continue;
      }

      const int pollResult = poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
      if (pollResult > 0 && (fds[0].revents & POLLIN) != 0) {
        (void)DrainEventFd(fds[0].fd);
        highWork = DrainQueue(QueueKind::HighRequest, highExecutor.get());
      }
      if (pollResult > 0 && !highWork && (fds[1].revents & POLLIN) != 0) {
        (void)DrainEventFd(fds[1].fd);
        DrainQueue(QueueKind::NormalRequest, normalExecutor.get());
      }
    }
  }
};

RpcServer::RpcServer() : impl_(std::make_unique<Impl>()) {}

RpcServer::RpcServer(BootstrapHandles handles, ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->options = std::move(options);
}

RpcServer::~RpcServer() {
  Stop();
}

void RpcServer::SetBootstrapHandles(BootstrapHandles handles) {
  impl_->handles = handles;
}

void RpcServer::RegisterHandler(Opcode opcode, RpcHandler handler) {
  if (!handler) {
    impl_->handlers.erase(static_cast<uint16_t>(opcode));
    return;
  }
  impl_->handlers[static_cast<uint16_t>(opcode)] = std::move(handler);
}

void RpcServer::SetOptions(ServerOptions options) {
  impl_->options = std::move(options);
}

StatusCode RpcServer::PublishEvent(const RpcEvent& event) {
  return impl_->PublishEvent(event);
}

StatusCode RpcServer::Start() {
  if (impl_->handlers.empty()) {
    return StatusCode::InvalidArgument;
  }
  if (impl_->running.exchange(true)) {
    return StatusCode::Ok;
  }

  const StatusCode attachStatus = impl_->session.Attach(impl_->handles, Session::AttachRole::Server);
  if (attachStatus != StatusCode::Ok) {
    impl_->running.store(false);
    return attachStatus;
  }

  impl_->highExecutor = impl_->options.highExecutor != nullptr
                            ? impl_->options.highExecutor
                            : std::make_shared<ThreadPoolExecutor>(impl_->options.highWorkerThreads);
  impl_->normalExecutor = impl_->options.normalExecutor != nullptr
                              ? impl_->options.normalExecutor
                              : std::make_shared<ThreadPoolExecutor>(impl_->options.normalWorkerThreads);
  impl_->completionQueueCapacity = impl_->options.completionQueueCapacity == 0
                                       ? impl_->session.Header()->responseRingSize
                                       : impl_->options.completionQueueCapacity;

  impl_->responseWriterRunning.store(true);
  impl_->responseWriterThread = std::thread([impl = impl_.get()] { impl->ResponseWriterLoop(); });
  impl_->dispatcherThread = std::thread([impl = impl_.get()] { impl->DispatcherLoop(); });
  return StatusCode::Ok;
}

void RpcServer::Run() {
  if (Start() != StatusCode::Ok) {
    return;
  }
  while (impl_->running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

RpcServerRuntimeStats RpcServer::GetRuntimeStats() const {
  RpcServerRuntimeStats stats;
  {
    std::lock_guard<std::mutex> lock(impl_->completionMutex);
    stats.completionBacklog = impl_->pendingCompletionCount;
    stats.completionBacklogCapacity = impl_->completionQueueCapacity;
  }
  if (impl_->session.Header() != nullptr) {
    stats.highRequestRingPending = RingCount(impl_->session.Header()->highRing);
    stats.normalRequestRingPending = RingCount(impl_->session.Header()->normalRing);
    stats.responseRingPending = RingCount(impl_->session.Header()->responseRing);
  }
  stats.waitingForResponseCredit = impl_->responseWriterWaitingForCredit.load(std::memory_order_acquire);
  return stats;
}

void RpcServer::Stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }

  impl_->StopResponseWriter();
  if (impl_->dispatcherThread.joinable()) {
    if (impl_->session.Handles().highReqEventFd >= 0) {
      (void)SignalEventFd(impl_->session.Handles().highReqEventFd);
    }
    if (impl_->session.Handles().normalReqEventFd >= 0) {
      (void)SignalEventFd(impl_->session.Handles().normalReqEventFd);
    }
    impl_->dispatcherThread.join();
  }
  if (impl_->highExecutor) {
    impl_->highExecutor->Stop();
    impl_->highExecutor.reset();
  }
  if (impl_->normalExecutor) {
    impl_->normalExecutor->Stop();
    impl_->normalExecutor.reset();
  }
  impl_->session.Reset();
}

}  // namespace MemRpc
