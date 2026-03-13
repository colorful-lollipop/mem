#include "memrpc/server/rpc_server.h"

#include <algorithm>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "memrpc/core/protocol.h"
#include "core/session.h"
#include "core/slot_pool.h"
#include "memrpc/core/task_executor.h"
#include "virus_protection_service_log.h"

namespace memrpc {

namespace {

uint32_t MonotonicNowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count() & 0xffffffffu);
}

uint32_t CurrentWorkerId() {
  return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) &
                               0xffffffffu);
}

bool RingCountIsOneAfterPush(const RingCursor& cursor) {
  const uint32_t tail = cursor.tail.load(std::memory_order_acquire);
  const uint32_t head = cursor.head.load(std::memory_order_acquire);
  return tail - head == 1u;
}

constexpr auto RESPONSE_RETRY_BUDGET = std::chrono::milliseconds(50);
constexpr auto EVENT_RETRY_BUDGET = std::chrono::milliseconds(10);

int64_t RemainingTimeoutMs(std::chrono::steady_clock::time_point deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
          .count();
  return remaining > 0 ? remaining : 0;
}

class ThreadPoolExecutor final : public TaskExecutor {
 public:
  explicit ThreadPoolExecutor(uint32_t threadCount) {
    const uint32_t threads = std::max(1u, threadCount);
    queue_capacity_ = threads;
    running_ = true;
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
      if (!running_ || queue_.size() >= queue_capacity_) {
        return false;
      }
      queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  bool HasCapacity() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && queue_.size() < queue_capacity_;
  }

  bool WaitForCapacity(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
      return !running_ || queue_.size() < queue_capacity_;
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
  uint32_t queue_capacity_ = 1;
};

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
    uint32_t requestSlotIndex = 0;
    std::vector<uint8_t> payload;
    std::chrono::milliseconds retryBudget{0};
    bool break_session_on_failure = false;
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
  uint32_t completion_queue_capacity = 0;
  uint32_t pending_completion_count = 0;
  std::thread response_writer_thread;
  std::atomic<bool> responseWriterRunning{false};
  std::atomic<bool> responseWriterWaitingForCredit{false};
  std::thread dispatcherThread;
  std::atomic<bool> running{false};
  std::unordered_map<uint16_t, RpcHandler> handlers;

  bool WaitForResponseCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.Handles().respCreditEventFd;
    if (fd < 0) {
      return false;
    }
    pollfd poll_fd{fd, POLLIN, 0};
    responseWriterWaitingForCredit.store(true);
    while (responseWriterRunning.load()) {
      const int64_t remaining_ms = RemainingTimeoutMs(deadline);
      if (remaining_ms <= 0) {
        responseWriterWaitingForCredit.store(false);
        return false;
      }
      const int poll_result = poll(&poll_fd, 1, static_cast<int>(remaining_ms));
      if (!responseWriterRunning.load()) {
        responseWriterWaitingForCredit.store(false);
        return false;
      }
      if (poll_result <= 0) {
        responseWriterWaitingForCredit.store(false);
        return false;
      }
      if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        responseWriterWaitingForCredit.store(false);
        return false;
      }
      if ((poll_fd.revents & POLLIN) == 0) {
        responseWriterWaitingForCredit.store(false);
        return false;
      }
      uint64_t counter = 0;
      bool drained = false;
      while (read(fd, &counter, sizeof(counter)) == sizeof(counter)) {
        drained = true;
      }
      responseWriterWaitingForCredit.store(false);
      return drained;
    }
    responseWriterWaitingForCredit.store(false);
    return false;
  }

  StatusCode PushResponseWithRetry(const ResponseRingEntry& response,
                                   std::chrono::milliseconds retry_budget) {
    const auto deadline = std::chrono::steady_clock::now() + retry_budget;
    while (true) {
      const StatusCode status = session.PushResponse(response);
      if (status == StatusCode::Ok || status != StatusCode::QueueFull) {
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return StatusCode::QueueFull;
      }
      if (!WaitForResponseCredit(deadline)) {
        return StatusCode::QueueFull;
      }
    }
  }

  std::optional<uint32_t> ReserveResponseSlotWithRetry(std::chrono::milliseconds retry_budget) {
    SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
    if (!response_slot_pool.Valid()) {
      return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + retry_budget;
    while (true) {
      const auto slot = response_slot_pool.Reserve();
      if (slot.has_value()) {
        return slot;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      if (!WaitForResponseCredit(deadline)) {
        return std::nullopt;
      }
    }
  }

  void MarkSessionBroken() {
    session.SetState(Session::SessionState::Broken);
    if (session.Handles().respEventFd < 0) {
      return;
    }
    const uint64_t signal_value = 1;
    write(session.Handles().respEventFd, &signal_value, sizeof(signal_value));
  }

  void CompleteItem(const std::shared_ptr<CompletionState>& completion, StatusCode status) {
    if (completion == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->status = status;
    completion->ready = true;
    completion->cv.notify_one();
  }

  void FailPendingCompletionItems(StatusCode status) {
    std::queue<CompletionItem> queued;
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      queued.swap(completionQueue);
      pending_completion_count = 0;
    }
    while (!queued.empty()) {
      CompleteItem(queued.front().completion, status);
      queued.pop();
    }
  }

  void StopResponseWriter() {
    responseWriterRunning.store(false);
    if (session.Handles().respCreditEventFd >= 0) {
      const uint64_t signal_value = 1;
      write(session.Handles().respCreditEventFd, &signal_value, sizeof(signal_value));
    }
    completionCv.notify_all();
    if (response_writer_thread.joinable()) {
      response_writer_thread.join();
    }
    FailPendingCompletionItems(StatusCode::PeerDisconnected);
  }

  void StartResponseWriter() {
    responseWriterRunning.store(true);
    response_writer_thread = std::thread([this] { ResponseWriterLoop(); });
  }

  bool EnqueueCompletion(CompletionItem item) {
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      if (completion_queue_capacity != 0 && pending_completion_count >= completion_queue_capacity) {
        return false;
      }
      ++pending_completion_count;
      completionQueue.push(std::move(item));
    }
    completionCv.notify_one();
    return true;
  }

  bool FillResponseSlot(uint32_t response_slot_index, CompletionItem& item) {
    ResponseSlotPayload* response_slot = session.GetResponseSlotPayload(response_slot_index);
    uint8_t* response_bytes = session.GetResponseSlotBytes(response_slot_index);
    if (response_slot == nullptr || response_bytes == nullptr ||
        item.payload.size() > session.Header()->maxResponseBytes) {
      SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
      response_slot_pool.Release(response_slot_index);
      CompleteItem(item.completion, StatusCode::ProtocolMismatch);
      if (item.break_session_on_failure) {
        MarkSessionBroken();
      }
      return false;
    }
    std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    response_slot->runtime.requestId = item.entry.requestId;
    response_slot->runtime.state = SlotRuntimeStateCode::Responding;
    response_slot->runtime.requestSlotIndex = item.requestSlotIndex;
    response_slot->runtime.lastUpdateMonoMs = MonotonicNowMs();
    response_slot->response.requestSlotIndex = item.requestSlotIndex;
    response_slot->response.payloadSize = static_cast<uint32_t>(item.payload.size());
    if (!item.payload.empty()) {
      std::memcpy(response_bytes, item.payload.data(), item.payload.size());
    }
    response_slot->runtime.state = SlotRuntimeStateCode::Ready;
    response_slot->runtime.publishMonoMs = MonotonicNowMs();
    response_slot->runtime.lastUpdateMonoMs = response_slot->runtime.publishMonoMs;
    item.entry.slotIndex = response_slot_index;
    item.entry.resultSize = static_cast<uint32_t>(item.payload.size());
    return true;
  }

  void PushResponseAndSignal(CompletionItem& item) {
    const StatusCode status = PushResponseWithRetry(item.entry, item.retryBudget);
    if (status == StatusCode::Ok) {
      if (RingCountIsOneAfterPush(session.Header()->responseRing)) {
        const uint64_t signal_value = 1;
        if (write(session.Handles().respEventFd, &signal_value, sizeof(signal_value)) !=
            sizeof(signal_value)) {
          CompleteItem(item.completion, StatusCode::PeerDisconnected);
          if (item.break_session_on_failure) {
            HILOGE("eventfd write failed while flushing response queue");
            MarkSessionBroken();
          }
          return;
        }
      }
    } else if (item.break_session_on_failure) {
      SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
      response_slot_pool.Release(item.entry.slotIndex);
      HILOGE("failed to enqueue rpc reply, status=%{public}d request_id=%{public}llu",
            static_cast<int>(status),
            static_cast<unsigned long long>(item.entry.requestId));
      MarkSessionBroken();
    } else {
      SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
      response_slot_pool.Release(item.entry.slotIndex);
      HILOGW("PushResponse for event failed, status=%{public}d", static_cast<int>(status));
    }
    CompleteItem(item.completion, status);
  }

  void ResponseWriterLoop() {
    while (responseWriterRunning.load()) {
      CompletionItem item;
      {
        std::unique_lock<std::mutex> lock(completionMutex);
        completionCv.wait(lock, [this] {
          return !responseWriterRunning.load(std::memory_order_relaxed) ||
                 !completionQueue.empty();
        });
        if (!responseWriterRunning.load() && completionQueue.empty()) {
          break;
      }
        item = std::move(completionQueue.front());
        completionQueue.pop();
      }

      const auto response_slot_index = ReserveResponseSlotWithRetry(item.retryBudget);
      if (!response_slot_index.has_value()) {
        CompleteItem(item.completion, StatusCode::QueueFull);
        if (item.break_session_on_failure) {
          HILOGE("failed to reserve response slot, request_id=%{public}llu",
                static_cast<unsigned long long>(item.entry.requestId));
          MarkSessionBroken();
        } else {
          HILOGW("failed to reserve response slot for event");
        }
        continue;
      }

      if (!FillResponseSlot(*response_slot_index, item)) {
        continue;
      }
      PushResponseAndSignal(item);
      {
        std::lock_guard<std::mutex> lock(completionMutex);
        if (pending_completion_count > 0) {
          --pending_completion_count;
        }
      }
    }
  }

  void WriteResponse(const RequestRingEntry& request_entry, const RpcServerReply& reply) {
    if (session.Header() == nullptr) {
      return;
    }

    SlotPayload* request_slot = session.GetSlotPayload(request_entry.slotIndex);
    if (request_slot != nullptr) {
      request_slot->runtime.state = SlotRuntimeStateCode::Responding;
      request_slot->runtime.lastHeartbeatMonoMs = MonotonicNowMs();
    }

    ResponseRingEntry response;
    response.requestId = request_entry.requestId;
    response.statusCode = static_cast<uint32_t>(reply.status);
    response.engineErrno = reply.engineCode;
    response.detailCode = reply.detailCode;
    if (reply.payload.size() > session.Header()->maxResponseBytes) {
      response.statusCode = static_cast<uint32_t>(StatusCode::EngineInternalError);
      response.engineErrno = 0;
      response.detailCode = 0;
    }
    CompletionItem item;
    item.entry = response;
    item.requestSlotIndex = request_entry.slotIndex;
    if (response.statusCode == static_cast<uint32_t>(StatusCode::EngineInternalError)) {
      item.payload.clear();
    } else {
      item.payload = reply.payload;
    }
    item.retryBudget = RESPONSE_RETRY_BUDGET;
    item.break_session_on_failure = true;
    if (!EnqueueCompletion(std::move(item))) {
      HILOGE("completion backlog full while enqueueing rpc reply, request_id=%{public}llu",
            static_cast<unsigned long long>(request_entry.requestId));
      MarkSessionBroken();
    }
  }

  StatusCode PublishEvent(const RpcEvent& event) {
    if (!session.Valid() || session.Handles().respEventFd < 0 || session.Header() == nullptr) {
      HILOGE("PublishEvent failed, session is not ready");
      return StatusCode::EngineInternalError;
    }
    if (event.payload.size() > session.Header()->maxResponseBytes) {
      HILOGW("PublishEvent payload too large, size=%{public}zu", event.payload.size());
      return StatusCode::InvalidArgument;
    }

    ResponseRingEntry entry;
    entry.messageKind = ResponseMessageKind::Event;
    entry.eventDomain = event.eventDomain;
    entry.eventType = event.eventType;
    entry.flags = event.flags;

    auto completion = std::make_shared<CompletionState>();
    CompletionItem item;
    item.entry = entry;
    item.payload = event.payload;
    item.retryBudget = EVENT_RETRY_BUDGET;
    item.completion = completion;
    if (!EnqueueCompletion(std::move(item))) {
      return StatusCode::QueueFull;
    }

    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->cv.wait(lock, [&completion] { return completion->ready; });
    return completion->status;
  }

  RpcServerCall BuildServerCall(const RequestRingEntry& request_entry,
                                const SlotPayload* payload) {
    RpcServerCall call;
    call.opcode = request_entry.opcode;
    call.priority = payload->request.priority == static_cast<uint32_t>(Priority::High)
                        ? Priority::High
                        : Priority::Normal;
    call.queueTimeoutMs = payload->request.queueTimeoutMs;
    call.execTimeoutMs = payload->request.execTimeoutMs;
    call.flags = payload->request.flags;
    call.payload = PayloadView(session.GetSlotRequestBytes(request_entry.slotIndex),
                               payload->request.payloadSize);
    return call;
  }

  RpcServerReply InvokeHandlerWithTimeout(const RequestRingEntry& request_entry,
                                           const SlotPayload* payload) {
    RpcServerReply reply;
    const auto it = handlers.find(request_entry.opcode);
    if (it == handlers.end()) {
      reply.status = StatusCode::InvalidArgument;
      return reply;
    }
    RpcServerCall call = BuildServerCall(request_entry, payload);
    const auto start = std::chrono::steady_clock::now();
    it->second(call, &reply);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (payload->request.execTimeoutMs > 0 &&
        elapsed > static_cast<long long>(payload->request.execTimeoutMs)) {
      reply.status = StatusCode::ExecTimeout;
    }
    return reply;
  }

  void ProcessEntry(const RequestRingEntry& request_entry) {
    SlotPayload* payload = session.GetSlotPayload(request_entry.slotIndex);
    uint8_t* request_bytes = session.GetSlotRequestBytes(request_entry.slotIndex);
    if (payload == nullptr || request_bytes == nullptr || session.Header() == nullptr) {
      return;
    }

    payload->runtime.requestId = request_entry.requestId;
    payload->runtime.state = SlotRuntimeStateCode::Executing;
    payload->runtime.workerId = CurrentWorkerId();
    payload->runtime.startExecMonoMs = MonotonicNowMs();
    payload->runtime.lastHeartbeatMonoMs = payload->runtime.startExecMonoMs;

    const uint32_t now_ms = MonotonicNowMs();
    if (payload->request.queueTimeoutMs > 0 &&
        now_ms - request_entry.enqueueMonoMs > payload->request.queueTimeoutMs) {
      RpcServerReply reply;
      reply.status = StatusCode::QueueTimeout;
      WriteResponse(request_entry, reply);
      return;
    }
    if (payload->request.payloadSize > session.Header()->maxRequestBytes) {
      RpcServerReply reply;
      reply.status = StatusCode::ProtocolMismatch;
      WriteResponse(request_entry, reply);
      return;
    }

    RpcServerReply reply = InvokeHandlerWithTimeout(request_entry, payload);
    WriteResponse(request_entry, reply);
  }

  bool DrainQueue(QueueKind kind, TaskExecutor* executor) {
    bool drained = false;
    RequestRingEntry entry;
    bool request_ring_became_not_full = false;
    while (executor->HasCapacity() && session.PopRequest(kind, &entry)) {
      const RingCursor& cursor =
          kind == QueueKind::HighRequest ? session.Header()->highRing : session.Header()->normalRing;
      if (cursor.capacity != 0 && RingCount(cursor) + 1u == cursor.capacity) {
        request_ring_became_not_full = true;
      }
      drained = true;
      const RequestRingEntry captured_entry = entry;
      executor->TrySubmit([this, captured_entry] { ProcessEntry(captured_entry); });
    }
    if (request_ring_became_not_full) {
      const uint64_t signal_value = 1;
      write(session.Handles().reqCreditEventFd, &signal_value, sizeof(signal_value));
    }
    return drained;
  }

  bool HandleBackloggedQueues() {
    const bool high_backlogged = session.Header() != nullptr &&
                                 RingCount(session.Header()->highRing) > 0 &&
                                 !highExecutor->HasCapacity();
    const bool normal_backlogged = session.Header() != nullptr &&
                                   RingCount(session.Header()->normalRing) > 0 &&
                                   !normalExecutor->HasCapacity();
    if (high_backlogged) {
      highExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    if (normal_backlogged) {
      normalExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    return false;
  }

  bool SpinForRingActivity(int iterations) {
    for (int i = 0; i < iterations; ++i) {
      if (session.Header() != nullptr &&
          (RingCount(session.Header()->highRing) > 0 ||
           RingCount(session.Header()->normalRing) > 0)) {
        return true;
      }
#if defined(__x86_64__) || defined(__i386__)
      __builtin_ia32_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#endif
    }
    return false;
  }

  void DispatcherLoop() {
    constexpr int SPIN_ITERATIONS = 256;
    pollfd fds[2] = {
        {session.Handles().highReqEventFd, POLLIN, 0},
        {session.Handles().normalReqEventFd, POLLIN, 0},
    };

    while (running.load()) {
      bool high_work = DrainQueue(QueueKind::HighRequest, highExecutor.get());
      if (!high_work) {
        DrainQueue(QueueKind::NormalRequest, normalExecutor.get());
      }

      if (HandleBackloggedQueues()) {
        continue;
      }
      if (SpinForRingActivity(SPIN_ITERATIONS)) {
        continue;
      }

      const int poll_result = poll(fds, 2, 100);
      uint64_t counter = 0;
      if (poll_result > 0 && (fds[0].revents & POLLIN) != 0) {
        while (read(fds[0].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        high_work = DrainQueue(QueueKind::HighRequest, highExecutor.get());
      }
      if (poll_result > 0 && !high_work && (fds[1].revents & POLLIN) != 0) {
        while (read(fds[1].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        DrainQueue(QueueKind::NormalRequest, normalExecutor.get());
      }
    }
  }
};

RpcServer::RpcServer() : impl_(std::make_unique<Impl>()) {}

RpcServer::RpcServer(BootstrapHandles handles, ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->options = options;
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
  impl_->options = options;
}

StatusCode RpcServer::PublishEvent(const RpcEvent& event) {
  return impl_->PublishEvent(event);
}

StatusCode RpcServer::Start() {
  if (impl_->handlers.empty()) {
    return StatusCode::InvalidArgument;
  }
  const StatusCode attach_status =
      impl_->session.Attach(impl_->handles, Session::AttachRole::Server);
  if (attach_status != StatusCode::Ok) {
    return attach_status;
  }
  impl_->running.store(true);
  impl_->completion_queue_capacity =
      impl_->options.completionQueueCapacity == 0
          ? std::max(1u, impl_->session.Header()->responseRingSize)
          : impl_->options.completionQueueCapacity;
  impl_->pending_completion_count = 0;
  impl_->StartResponseWriter();
  impl_->highExecutor = impl_->options.highExecutor
      ? impl_->options.highExecutor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.highWorkerThreads);
  impl_->normalExecutor = impl_->options.normalExecutor
      ? impl_->options.normalExecutor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.normalWorkerThreads);
  impl_->dispatcherThread = std::thread([this] { impl_->DispatcherLoop(); });
  return StatusCode::Ok;
}

void RpcServer::Run() {
  if (!impl_->running.load()) {
    if (Start() != StatusCode::Ok) {
      return;
    }
  }
  while (impl_->running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

RpcServerRuntimeStats RpcServer::GetRuntimeStats() const {
  RpcServerRuntimeStats stats;
  if (impl_ == nullptr) {
    return stats;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->completionMutex);
    stats.completionBacklog = impl_->pending_completion_count;
    stats.completionBacklogCapacity = impl_->completion_queue_capacity;
  }
  if (impl_->session.Header() != nullptr) {
    stats.highRequestRingPending = RingCount(impl_->session.Header()->highRing);
    stats.normalRequestRingPending = RingCount(impl_->session.Header()->normalRing);
    stats.responseRingPending = RingCount(impl_->session.Header()->responseRing);
  }
  stats.waitingForResponseCredit =
      impl_->responseWriterWaitingForCredit.load(std::memory_order_relaxed);
  return stats;
}

void RpcServer::Stop() {
  impl_->running.store(false);
  if (impl_->dispatcherThread.joinable()) {
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
  impl_->StopResponseWriter();
  impl_->session.Reset();
}

}  // namespace memrpc
