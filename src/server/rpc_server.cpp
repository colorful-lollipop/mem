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

#include "core/protocol.h"
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
  explicit ThreadPoolExecutor(uint32_t thread_count) {
    const uint32_t threads = std::max(1u, thread_count);
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
    uint32_t request_slot_index = 0;
    std::vector<uint8_t> payload;
    std::chrono::milliseconds retry_budget{0};
    bool break_session_on_failure = false;
    std::shared_ptr<CompletionState> completion;
  };

  BootstrapHandles handles{};
  ServerOptions options{};
  Session session;
  std::shared_ptr<TaskExecutor> high_executor;
  std::shared_ptr<TaskExecutor> normal_executor;
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::queue<CompletionItem> completion_queue;
  uint32_t completion_queue_capacity = 0;
  uint32_t pending_completion_count = 0;
  std::thread response_writer_thread;
  std::atomic<bool> response_writer_running{false};
  std::atomic<bool> response_writer_waiting_for_credit{false};
  std::thread dispatcher_thread;
  std::atomic<bool> running{false};
  std::unordered_map<uint16_t, RpcHandler> handlers;

  bool WaitForResponseCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.handles().resp_credit_event_fd;
    if (fd < 0) {
      return false;
    }
    pollfd poll_fd{fd, POLLIN, 0};
    response_writer_waiting_for_credit.store(true);
    while (response_writer_running.load()) {
      const int64_t remaining_ms = RemainingTimeoutMs(deadline);
      if (remaining_ms <= 0) {
        response_writer_waiting_for_credit.store(false);
        return false;
      }
      const int poll_result = poll(&poll_fd, 1, static_cast<int>(remaining_ms));
      if (!response_writer_running.load()) {
        response_writer_waiting_for_credit.store(false);
        return false;
      }
      if (poll_result <= 0) {
        response_writer_waiting_for_credit.store(false);
        return false;
      }
      if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        response_writer_waiting_for_credit.store(false);
        return false;
      }
      if ((poll_fd.revents & POLLIN) == 0) {
        response_writer_waiting_for_credit.store(false);
        return false;
      }
      uint64_t counter = 0;
      bool drained = false;
      while (read(fd, &counter, sizeof(counter)) == sizeof(counter)) {
        drained = true;
      }
      response_writer_waiting_for_credit.store(false);
      return drained;
    }
    response_writer_waiting_for_credit.store(false);
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
    SharedSlotPool response_slot_pool(session.response_slot_pool_region());
    if (!response_slot_pool.valid()) {
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
    if (session.handles().resp_event_fd < 0) {
      return;
    }
    const uint64_t signal_value = 1;
    write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value));
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
      std::lock_guard<std::mutex> lock(completion_mutex);
      queued.swap(completion_queue);
      pending_completion_count = 0;
    }
    while (!queued.empty()) {
      CompleteItem(queued.front().completion, status);
      queued.pop();
    }
  }

  void StopResponseWriter() {
    response_writer_running.store(false);
    if (session.handles().resp_credit_event_fd >= 0) {
      const uint64_t signal_value = 1;
      write(session.handles().resp_credit_event_fd, &signal_value, sizeof(signal_value));
    }
    completion_cv.notify_all();
    if (response_writer_thread.joinable()) {
      response_writer_thread.join();
    }
    FailPendingCompletionItems(StatusCode::PeerDisconnected);
  }

  void StartResponseWriter() {
    response_writer_running.store(true);
    response_writer_thread = std::thread([this] { ResponseWriterLoop(); });
  }

  bool EnqueueCompletion(CompletionItem item) {
    {
      std::lock_guard<std::mutex> lock(completion_mutex);
      if (completion_queue_capacity != 0 && pending_completion_count >= completion_queue_capacity) {
        return false;
      }
      ++pending_completion_count;
      completion_queue.push(std::move(item));
    }
    completion_cv.notify_one();
    return true;
  }

  bool FillResponseSlot(uint32_t response_slot_index, CompletionItem& item) {
    ResponseSlotPayload* response_slot = session.response_slot_payload(response_slot_index);
    uint8_t* response_bytes = session.response_slot_bytes(response_slot_index);
    if (response_slot == nullptr || response_bytes == nullptr ||
        item.payload.size() > session.header()->max_response_bytes) {
      SharedSlotPool response_slot_pool(session.response_slot_pool_region());
      response_slot_pool.Release(response_slot_index);
      CompleteItem(item.completion, StatusCode::ProtocolMismatch);
      if (item.break_session_on_failure) {
        MarkSessionBroken();
      }
      return false;
    }
    std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    response_slot->runtime.request_id = item.entry.request_id;
    response_slot->runtime.state = SlotRuntimeStateCode::Responding;
    response_slot->runtime.request_slot_index = item.request_slot_index;
    response_slot->runtime.last_update_mono_ms = MonotonicNowMs();
    response_slot->response.request_slot_index = item.request_slot_index;
    response_slot->response.payload_size = static_cast<uint32_t>(item.payload.size());
    if (!item.payload.empty()) {
      std::memcpy(response_bytes, item.payload.data(), item.payload.size());
    }
    response_slot->runtime.state = SlotRuntimeStateCode::Ready;
    response_slot->runtime.publish_mono_ms = MonotonicNowMs();
    response_slot->runtime.last_update_mono_ms = response_slot->runtime.publish_mono_ms;
    item.entry.slot_index = response_slot_index;
    item.entry.result_size = static_cast<uint32_t>(item.payload.size());
    return true;
  }

  void PushResponseAndSignal(CompletionItem& item) {
    const StatusCode status = PushResponseWithRetry(item.entry, item.retry_budget);
    if (status == StatusCode::Ok) {
      if (RingCountIsOneAfterPush(session.header()->response_ring)) {
        const uint64_t signal_value = 1;
        if (write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value)) !=
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
      SharedSlotPool response_slot_pool(session.response_slot_pool_region());
      response_slot_pool.Release(item.entry.slot_index);
      HILOGE("failed to enqueue rpc reply, status=%{public}d request_id=%{public}llu",
            static_cast<int>(status),
            static_cast<unsigned long long>(item.entry.request_id));
      MarkSessionBroken();
    } else {
      SharedSlotPool response_slot_pool(session.response_slot_pool_region());
      response_slot_pool.Release(item.entry.slot_index);
      HILOGW("PushResponse for event failed, status=%{public}d", static_cast<int>(status));
    }
    CompleteItem(item.completion, status);
  }

  void ResponseWriterLoop() {
    while (response_writer_running.load()) {
      CompletionItem item;
      {
        std::unique_lock<std::mutex> lock(completion_mutex);
        completion_cv.wait(lock, [this] {
          return !response_writer_running.load(std::memory_order_relaxed) ||
                 !completion_queue.empty();
        });
        if (!response_writer_running.load() && completion_queue.empty()) {
          break;
      }
        item = std::move(completion_queue.front());
        completion_queue.pop();
      }

      const auto response_slot_index = ReserveResponseSlotWithRetry(item.retry_budget);
      if (!response_slot_index.has_value()) {
        CompleteItem(item.completion, StatusCode::QueueFull);
        if (item.break_session_on_failure) {
          HILOGE("failed to reserve response slot, request_id=%{public}llu",
                static_cast<unsigned long long>(item.entry.request_id));
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
        std::lock_guard<std::mutex> lock(completion_mutex);
        if (pending_completion_count > 0) {
          --pending_completion_count;
        }
      }
    }
  }

  void WriteResponse(const RequestRingEntry& request_entry, const RpcServerReply& reply) {
    if (session.header() == nullptr) {
      return;
    }

    SlotPayload* request_slot = session.slot_payload(request_entry.slot_index);
    if (request_slot != nullptr) {
      request_slot->runtime.state = SlotRuntimeStateCode::Responding;
      request_slot->runtime.last_heartbeat_mono_ms = MonotonicNowMs();
    }

    ResponseRingEntry response;
    response.request_id = request_entry.request_id;
    response.status_code = static_cast<uint32_t>(reply.status);
    response.engine_errno = reply.engine_code;
    response.detail_code = reply.detail_code;
    if (reply.payload.size() > session.header()->max_response_bytes) {
      response.status_code = static_cast<uint32_t>(StatusCode::EngineInternalError);
      response.engine_errno = 0;
      response.detail_code = 0;
    }
    CompletionItem item;
    item.entry = response;
    item.request_slot_index = request_entry.slot_index;
    if (response.status_code == static_cast<uint32_t>(StatusCode::EngineInternalError)) {
      item.payload.clear();
    } else {
      item.payload = reply.payload;
    }
    item.retry_budget = RESPONSE_RETRY_BUDGET;
    item.break_session_on_failure = true;
    if (!EnqueueCompletion(std::move(item))) {
      HILOGE("completion backlog full while enqueueing rpc reply, request_id=%{public}llu",
            static_cast<unsigned long long>(request_entry.request_id));
      MarkSessionBroken();
    }
  }

  StatusCode PublishEvent(const RpcEvent& event) {
    if (!session.valid() || session.handles().resp_event_fd < 0 || session.header() == nullptr) {
      HILOGE("PublishEvent failed, session is not ready");
      return StatusCode::EngineInternalError;
    }
    if (event.payload.size() > session.header()->max_response_bytes) {
      HILOGW("PublishEvent payload too large, size=%{public}zu", event.payload.size());
      return StatusCode::InvalidArgument;
    }

    ResponseRingEntry entry;
    entry.message_kind = ResponseMessageKind::Event;
    entry.event_domain = event.event_domain;
    entry.event_type = event.event_type;
    entry.flags = event.flags;

    auto completion = std::make_shared<CompletionState>();
    CompletionItem item;
    item.entry = entry;
    item.payload = event.payload;
    item.retry_budget = EVENT_RETRY_BUDGET;
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
    call.queue_timeout_ms = payload->request.queue_timeout_ms;
    call.exec_timeout_ms = payload->request.exec_timeout_ms;
    call.flags = payload->request.flags;
    call.payload = PayloadView(session.slot_request_bytes(request_entry.slot_index),
                               payload->request.payload_size);
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
    if (payload->request.exec_timeout_ms > 0 &&
        elapsed > static_cast<long long>(payload->request.exec_timeout_ms)) {
      reply.status = StatusCode::ExecTimeout;
    }
    return reply;
  }

  void ProcessEntry(const RequestRingEntry& request_entry) {
    SlotPayload* payload = session.slot_payload(request_entry.slot_index);
    uint8_t* request_bytes = session.slot_request_bytes(request_entry.slot_index);
    if (payload == nullptr || request_bytes == nullptr || session.header() == nullptr) {
      return;
    }

    payload->runtime.request_id = request_entry.request_id;
    payload->runtime.state = SlotRuntimeStateCode::Executing;
    payload->runtime.worker_id = CurrentWorkerId();
    payload->runtime.start_exec_mono_ms = MonotonicNowMs();
    payload->runtime.last_heartbeat_mono_ms = payload->runtime.start_exec_mono_ms;

    const uint32_t now_ms = MonotonicNowMs();
    if (payload->request.queue_timeout_ms > 0 &&
        now_ms - request_entry.enqueue_mono_ms > payload->request.queue_timeout_ms) {
      RpcServerReply reply;
      reply.status = StatusCode::QueueTimeout;
      WriteResponse(request_entry, reply);
      return;
    }
    if (payload->request.payload_size > session.header()->max_request_bytes) {
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
          kind == QueueKind::HighRequest ? session.header()->high_ring : session.header()->normal_ring;
      if (cursor.capacity != 0 && RingCount(cursor) + 1u == cursor.capacity) {
        request_ring_became_not_full = true;
      }
      drained = true;
      const RequestRingEntry captured_entry = entry;
      executor->TrySubmit([this, captured_entry] { ProcessEntry(captured_entry); });
    }
    if (request_ring_became_not_full) {
      const uint64_t signal_value = 1;
      write(session.handles().req_credit_event_fd, &signal_value, sizeof(signal_value));
    }
    return drained;
  }

  bool HandleBackloggedQueues() {
    const bool high_backlogged = session.header() != nullptr &&
                                 RingCount(session.header()->high_ring) > 0 &&
                                 !high_executor->HasCapacity();
    const bool normal_backlogged = session.header() != nullptr &&
                                   RingCount(session.header()->normal_ring) > 0 &&
                                   !normal_executor->HasCapacity();
    if (high_backlogged) {
      high_executor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    if (normal_backlogged) {
      normal_executor->WaitForCapacity(std::chrono::milliseconds(100));
      return true;
    }
    return false;
  }

  bool SpinForRingActivity(int iterations) {
    for (int i = 0; i < iterations; ++i) {
      if (session.header() != nullptr &&
          (RingCount(session.header()->high_ring) > 0 ||
           RingCount(session.header()->normal_ring) > 0)) {
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
        {session.handles().high_req_event_fd, POLLIN, 0},
        {session.handles().normal_req_event_fd, POLLIN, 0},
    };

    while (running.load()) {
      bool high_work = DrainQueue(QueueKind::HighRequest, high_executor.get());
      if (!high_work) {
        DrainQueue(QueueKind::NormalRequest, normal_executor.get());
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
        high_work = DrainQueue(QueueKind::HighRequest, high_executor.get());
      }
      if (poll_result > 0 && !high_work && (fds[1].revents & POLLIN) != 0) {
        while (read(fds[1].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        DrainQueue(QueueKind::NormalRequest, normal_executor.get());
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
      impl_->options.completion_queue_capacity == 0
          ? std::max(1u, impl_->session.header()->response_ring_size)
          : impl_->options.completion_queue_capacity;
  impl_->pending_completion_count = 0;
  impl_->StartResponseWriter();
  impl_->high_executor = impl_->options.high_executor
      ? impl_->options.high_executor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.high_worker_threads);
  impl_->normal_executor = impl_->options.normal_executor
      ? impl_->options.normal_executor
      : std::make_shared<ThreadPoolExecutor>(impl_->options.normal_worker_threads);
  impl_->dispatcher_thread = std::thread([this] { impl_->DispatcherLoop(); });
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
    std::lock_guard<std::mutex> lock(impl_->completion_mutex);
    stats.completion_backlog = impl_->pending_completion_count;
    stats.completion_backlog_capacity = impl_->completion_queue_capacity;
  }
  if (impl_->session.header() != nullptr) {
    stats.high_request_ring_pending = RingCount(impl_->session.header()->high_ring);
    stats.normal_request_ring_pending = RingCount(impl_->session.header()->normal_ring);
    stats.response_ring_pending = RingCount(impl_->session.header()->response_ring);
  }
  stats.waiting_for_response_credit =
      impl_->response_writer_waiting_for_credit.load(std::memory_order_relaxed);
  return stats;
}

void RpcServer::Stop() {
  impl_->running.store(false);
  if (impl_->dispatcher_thread.joinable()) {
    impl_->dispatcher_thread.join();
  }
  if (impl_->high_executor) {
    impl_->high_executor->Stop();
    impl_->high_executor.reset();
  }
  if (impl_->normal_executor) {
    impl_->normal_executor->Stop();
    impl_->normal_executor.reset();
  }
  impl_->StopResponseWriter();
  impl_->session.Reset();
}

}  // namespace memrpc
