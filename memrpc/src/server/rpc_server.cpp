#include "memrpc/server/rpc_server.h"

#include <algorithm>
#include <array>
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
#include <unordered_set>
#include <utility>
#include <vector>

#include "memrpc/core/protocol.h"
#include "core/session.h"
#include "core/slot_pool.h"
#include "memrpc/core/runtime_utils.h"
#include "memrpc/core/task_executor.h"
#include "virus_protection_service_log.h"

namespace MemRpc {

namespace {

uint32_t CurrentWorkerId() {
  return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) &
                               0xffffffffU);
}

constexpr auto RESPONSE_RETRY_BUDGET = std::chrono::milliseconds(50);
constexpr auto EVENT_RETRY_BUDGET = std::chrono::milliseconds(10);

class ThreadPoolExecutor final : public TaskExecutor {
 public:
  explicit ThreadPoolExecutor(uint32_t threadCount) : running_(true) {
    const uint32_t threads = std::max(1U, threadCount);
    queue_capacity_ = threads;
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
  std::thread executionHeartbeatThread;
  std::atomic<bool> running{false};
  std::unordered_map<uint16_t, RpcHandler> handlers;
  std::mutex executingSlotsMutex;
  std::unordered_set<uint32_t> executingSlots;

  void RegisterExecutingSlot(uint32_t request_slot_index) {
    std::lock_guard<std::mutex> lock(executingSlotsMutex);
    executingSlots.insert(request_slot_index);
  }

  void UnregisterExecutingSlot(uint32_t request_slot_index) {
    std::lock_guard<std::mutex> lock(executingSlotsMutex);
    executingSlots.erase(request_slot_index);
  }

  void StartExecutionHeartbeat() {
    executionHeartbeatThread = std::thread([this] { ExecutionHeartbeatLoop(); });
  }

  void StopExecutionHeartbeat() {
    if (executionHeartbeatThread.joinable()) {
      executionHeartbeatThread.join();
    }
    std::lock_guard<std::mutex> lock(executingSlotsMutex);
    executingSlots.clear();
  }

  void ExecutionHeartbeatLoop() {
    constexpr auto HEARTBEAT_INTERVAL = std::chrono::milliseconds(25);
    while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(HEARTBEAT_INTERVAL);
      if (!running.load(std::memory_order_acquire)) {
        break;
      }

      std::vector<uint32_t> active_slots;
      {
        std::lock_guard<std::mutex> lock(executingSlotsMutex);
        active_slots.assign(executingSlots.begin(), executingSlots.end());
      }

      const uint32_t now_ms = MonotonicNowMs();
      for (uint32_t slot_index : active_slots) {
        SlotPayload* request_slot = session.GetSlotPayload(slot_index);
        if (request_slot == nullptr) {
          continue;
        }
        request_slot->runtime.lastHeartbeatMonoMs = now_ms;
      }
    }
  }

  PollEventFdResult WaitForResponseCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.Handles().respCreditEventFd;
    if (fd < 0) {
      return PollEventFdResult::Failed;
    }
    pollfd poll_fd{fd, POLLIN, 0};
    responseWriterWaitingForCredit.store(true);
    [[maybe_unused]] const auto clear_waiting =
        MakeScopeExit([this] { responseWriterWaitingForCredit.store(false); });
    while (responseWriterRunning.load()) {
      const int64_t remaining_ms = RemainingTimeoutMs(deadline);
      if (remaining_ms <= 0) {
        return PollEventFdResult::Timeout;
      }
      const auto wait_result = PollEventFd(&poll_fd, static_cast<int>(remaining_ms));
      if (wait_result == PollEventFdResult::Retry) {
        continue;
      }
      return wait_result;
    }
    return PollEventFdResult::Failed;
  }

  StatusCode PushResponseWithRetry(const ResponseRingEntry& response,
                                   std::chrono::milliseconds retry_budget) {
    const auto deadline = std::chrono::steady_clock::now() + retry_budget;
    while (true) {
      const StatusCode status = session.PushResponse(response);
      if (status == StatusCode::Ok || status != StatusCode::QueueFull) {
        return status;
      }
      if (DeadlineReached(deadline)) {
        return StatusCode::QueueFull;
      }
      const auto wait_result = WaitForResponseCredit(deadline);
      if (wait_result == PollEventFdResult::Ready) {
        continue;
      }
      return wait_result == PollEventFdResult::Timeout ? StatusCode::QueueFull
                                                       : StatusCode::PeerDisconnected;
    }
  }

  StatusCode ReserveResponseSlotWithRetry(std::chrono::milliseconds retry_budget,
                                          uint32_t* response_slot_index) {
    SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
    if (response_slot_index == nullptr || !response_slot_pool.Valid()) {
      return StatusCode::PeerDisconnected;
    }

    const auto deadline = std::chrono::steady_clock::now() + retry_budget;
    while (true) {
      const auto slot = response_slot_pool.Reserve();
      if (slot.has_value()) {
        *response_slot_index = *slot;
        return StatusCode::Ok;
      }
      if (DeadlineReached(deadline)) {
        return StatusCode::QueueFull;
      }
      const auto wait_result = WaitForResponseCredit(deadline);
      if (wait_result != PollEventFdResult::Ready) {
        return wait_result == PollEventFdResult::Timeout ? StatusCode::QueueFull
                                                         : StatusCode::PeerDisconnected;
      }
    }
  }

  void MarkSessionBroken() {
    session.SetState(Session::SessionState::Broken);
    if (session.Handles().respEventFd < 0) {
      return;
    }
    (void)SignalEventFd(session.Handles().respEventFd);
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
    if (pending_completion_count > 0) {
      --pending_completion_count;
    }
  }

  bool WaitAndPopCompletionItem(CompletionItem* item) {
    if (item == nullptr) {
      return false;
    }
    std::unique_lock<std::mutex> lock(completionMutex);
    completionCv.wait(lock, [this] {
      return !responseWriterRunning.load(std::memory_order_relaxed) ||
             !completionQueue.empty();
    });
    if (!responseWriterRunning.load() && completionQueue.empty()) {
      return false;
    }
    *item = std::move(completionQueue.front());
    completionQueue.pop();
    return true;
  }

  void ReleaseResponseSlot(uint32_t response_slot_index) {
    SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
    response_slot_pool.Release(response_slot_index);
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
      (void)SignalEventFd(session.Handles().respCreditEventFd);
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
      ReleaseResponseSlot(response_slot_index);
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

  bool SignalResponseReady() const {
    if (!RingCountIsOneAfterPush(session.Header()->responseRing)) {
      return true;
    }
    return SignalEventFd(session.Handles().respEventFd);
  }

  void HandleResponsePushFailure(const CompletionItem& item, StatusCode status) {
    ReleaseResponseSlot(item.entry.slotIndex);
    if (status == StatusCode::PeerDisconnected) {
      HILOGE("failed to publish response, peer disconnected");
    } else if (item.break_session_on_failure) {
      HILOGE("failed to enqueue rpc reply, status=%{public}d request_id=%{public}llu",
             static_cast<int>(status),
             static_cast<unsigned long long>(item.entry.requestId));
    } else {
      HILOGW("PushResponse for event failed, status=%{public}d", static_cast<int>(status));
    }
    if (item.break_session_on_failure) {
      MarkSessionBroken();
    }
  }

  void PushResponseAndSignal(CompletionItem& item) {
    const StatusCode status = PushResponseWithRetry(item.entry, item.retryBudget);
    if (status != StatusCode::Ok) {
      HandleResponsePushFailure(item, status);
      CompleteItem(item.completion, status);
      return;
    }
    if (!SignalResponseReady()) {
      HILOGW("response published without wakeup signal, request_id=%{public}llu slot=%{public}u",
             static_cast<unsigned long long>(item.entry.requestId), item.entry.slotIndex);
    }
    CompleteItem(item.completion, StatusCode::Ok);
  }

  void HandleResponseSlotReservationFailure(const CompletionItem& item, StatusCode status) {
    CompleteItem(item.completion, status);
    if (!item.break_session_on_failure) {
      HILOGW("failed to reserve response slot for event, status=%{public}d",
             static_cast<int>(status));
      return;
    }
    HILOGE("failed to reserve response slot, request_id=%{public}llu status=%{public}d",
           static_cast<unsigned long long>(item.entry.requestId), static_cast<int>(status));
    MarkSessionBroken();
  }

  void ResponseWriterLoop() {
    while (responseWriterRunning.load()) {
      CompletionItem item;
      if (!WaitAndPopCompletionItem(&item)) {
        break;
      }
      [[maybe_unused]] const auto complete_guard =
          MakeScopeExit([this] { OnCompletionItemFinished(); });

      uint32_t response_slot_index = 0;
      const StatusCode reserve_status =
          ReserveResponseSlotWithRetry(item.retryBudget, &response_slot_index);
      if (reserve_status != StatusCode::Ok) {
        HandleResponseSlotReservationFailure(item, reserve_status);
        continue;
      }

      if (!FillResponseSlot(response_slot_index, item)) {
        continue;
      }
      PushResponseAndSignal(item);
    }
  }

  void MarkRequestSlotResponding(uint32_t request_slot_index) {
    SlotPayload* request_slot = session.GetSlotPayload(request_slot_index);
    if (request_slot == nullptr) {
      return;
    }
    request_slot->runtime.state = SlotRuntimeStateCode::Responding;
    request_slot->runtime.lastHeartbeatMonoMs = MonotonicNowMs();
  }

  CompletionItem BuildResponseCompletionItem(const RequestRingEntry& request_entry,
                                             const RpcServerReply& reply) const {
    CompletionItem item;
    item.entry.requestId = request_entry.requestId;
    item.entry.statusCode = static_cast<uint32_t>(reply.status);
    item.entry.errorCode = reply.errorCode;
    item.requestSlotIndex = request_entry.slotIndex;
    item.retryBudget = RESPONSE_RETRY_BUDGET;
    item.break_session_on_failure = true;
    if (reply.payload.size() > session.Header()->maxResponseBytes) {
      item.entry.statusCode = static_cast<uint32_t>(StatusCode::EngineInternalError);
      item.entry.errorCode = 0;
      return item;
    }
    item.payload = reply.payload;
    return item;
  }

  void WriteResponse(const RequestRingEntry& request_entry, const RpcServerReply& reply) {
    if (session.Header() == nullptr) {
      return;
    }
    MarkRequestSlotResponding(request_entry.slotIndex);
    CompletionItem item = BuildResponseCompletionItem(request_entry, reply);
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
    RegisterExecutingSlot(request_entry.slotIndex);
    [[maybe_unused]] const auto unregister_guard =
        MakeScopeExit([this, request_slot_index = request_entry.slotIndex] {
          UnregisterExecutingSlot(request_slot_index);
        });
    const auto start = std::chrono::steady_clock::now();
    it->second(call, &reply);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (payload->request.execTimeoutMs > 0 &&
        elapsed > static_cast<long long>(payload->request.execTimeoutMs)) {
      HILOGE("exec timeout request_id=%{public}llu slot=%{public}u opcode=%{public}u elapsed_ms=%{public}lld timeout_ms=%{public}u",
             static_cast<unsigned long long>(request_entry.requestId),
             request_entry.slotIndex,
             static_cast<unsigned>(request_entry.opcode),
             static_cast<long long>(elapsed),
             payload->request.execTimeoutMs);
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
      if (cursor.capacity != 0 && RingCount(cursor) + 1U == cursor.capacity) {
        request_ring_became_not_full = true;
      }
      drained = true;
      const RequestRingEntry captured_entry = entry;
      (void)executor->TrySubmit([this, captured_entry] { ProcessEntry(captured_entry); });
    }
    if (request_ring_became_not_full) {
      (void)SignalEventFd(session.Handles().reqCreditEventFd);
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
      (void)highExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    if (normal_backlogged) {
      (void)normalExecutor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    return false;
  }

  bool SpinForRingActivity(int iterations) const {
    for (int i = 0; i < iterations; ++i) {
      if (session.Header() != nullptr &&
          (RingCount(session.Header()->highRing) > 0 ||
           RingCount(session.Header()->normalRing) > 0)) {
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

      const int poll_result =
          poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
      if (poll_result > 0 && (fds[0].revents & POLLIN) != 0) {
        (void)DrainEventFd(fds[0].fd);
        high_work = DrainQueue(QueueKind::HighRequest, highExecutor.get());
      }
      if (poll_result > 0 && !high_work && (fds[1].revents & POLLIN) != 0) {
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
  const StatusCode attach_status =
      impl_->session.Attach(impl_->handles, Session::AttachRole::Server);
  if (attach_status != StatusCode::Ok) {
    return attach_status;
  }
  impl_->running.store(true);
  impl_->completion_queue_capacity =
      impl_->options.completionQueueCapacity == 0
          ? std::max(1U, impl_->session.Header()->responseRingSize)
          : impl_->options.completionQueueCapacity;
  impl_->pending_completion_count = 0;
  impl_->StartResponseWriter();
  impl_->highExecutor = impl_->options.highExecutor
      ? impl_->options.highExecutor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.highWorkerThreads);
  impl_->normalExecutor = impl_->options.normalExecutor
      ? impl_->options.normalExecutor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.normalWorkerThreads);
  impl_->StartExecutionHeartbeat();
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
  impl_->StopExecutionHeartbeat();
  impl_->StopResponseWriter();
  impl_->session.Reset();
}

}  // namespace MemRpc
