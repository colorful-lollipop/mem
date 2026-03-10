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
  const uint32_t tail = __atomic_load_n(&cursor.tail, __ATOMIC_ACQUIRE);
  const uint32_t head = __atomic_load_n(&cursor.head, __ATOMIC_ACQUIRE);
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

class WorkerPool {
 public:
  void Start(uint32_t thread_count, std::function<void(const RequestRingEntry&)> callback) {
    callback_ = std::move(callback);
    running_ = true;
    queue_capacity_ = std::max(1u, thread_count);
    for (uint32_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  bool HasCapacity() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() < queue_capacity_;
  }

  void Enqueue(const RequestRingEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(entry);
    cv_.notify_one();
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
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

  bool WaitForCapacity(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
      return !running_ || queue_.size() < queue_capacity_;
    });
  }

 private:
  void WorkerLoop() {
    while (true) {
      RequestRingEntry entry;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) {
          return;
        }
        entry = queue_.front();
        queue_.pop();
        cv_.notify_all();
      }
      callback_(entry);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<RequestRingEntry> queue_;
  std::vector<std::thread> workers_;
  std::function<void(const RequestRingEntry&)> callback_;
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
  WorkerPool high_pool;
  WorkerPool normal_pool;
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
          HLOGE("failed to reserve response slot, request_id=%{public}llu",
                static_cast<unsigned long long>(item.entry.request_id));
          MarkSessionBroken();
        } else {
          HLOGW("failed to reserve response slot for event");
        }
        continue;
      }

      ResponseSlotPayload* response_slot = session.response_slot_payload(*response_slot_index);
      uint8_t* response_bytes = session.response_slot_bytes(*response_slot_index);
      if (response_slot == nullptr || response_bytes == nullptr ||
          item.payload.size() > session.header()->max_response_bytes) {
        SharedSlotPool response_slot_pool(session.response_slot_pool_region());
        response_slot_pool.Release(*response_slot_index);
        CompleteItem(item.completion, StatusCode::ProtocolMismatch);
        if (item.break_session_on_failure) {
          MarkSessionBroken();
        }
        continue;
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
      item.entry.slot_index = *response_slot_index;
      item.entry.result_size = static_cast<uint32_t>(item.payload.size());

      const StatusCode status = PushResponseWithRetry(item.entry, item.retry_budget);
      if (status == StatusCode::Ok) {
        if (RingCountIsOneAfterPush(session.header()->response_ring)) {
          const uint64_t signal_value = 1;
          if (write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value)) !=
              sizeof(signal_value)) {
            CompleteItem(item.completion, StatusCode::PeerDisconnected);
            if (item.break_session_on_failure) {
              HLOGE("eventfd write failed while flushing response queue");
              MarkSessionBroken();
            }
            continue;
          }
        }
      } else if (item.break_session_on_failure) {
        SharedSlotPool response_slot_pool(session.response_slot_pool_region());
        response_slot_pool.Release(*response_slot_index);
        HLOGE("failed to enqueue rpc reply, status=%{public}d request_id=%{public}llu",
              static_cast<int>(status),
              static_cast<unsigned long long>(item.entry.request_id));
        MarkSessionBroken();
      } else {
        SharedSlotPool response_slot_pool(session.response_slot_pool_region());
        response_slot_pool.Release(*response_slot_index);
        HLOGW("PushResponse for event failed, status=%{public}d", static_cast<int>(status));
      }

      CompleteItem(item.completion, status);
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
      HLOGE("completion backlog full while enqueueing rpc reply, request_id=%{public}llu",
            static_cast<unsigned long long>(request_entry.request_id));
      MarkSessionBroken();
    }
  }

  StatusCode PublishEvent(const RpcEvent& event) {
    if (!session.valid() || session.handles().resp_event_fd < 0 || session.header() == nullptr) {
      HLOGE("PublishEvent failed, session is not ready");
      return StatusCode::EngineInternalError;
    }
    if (event.payload.size() > session.header()->max_response_bytes) {
      HLOGW("PublishEvent payload too large, size=%{public}zu", event.payload.size());
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

    RpcServerReply reply;
    const uint32_t now_ms = MonotonicNowMs();
    // queue_timeout 只覆盖“进入 worker 前”的排队时长，不包含 handler 执行时间。
    if (payload->request.queue_timeout_ms > 0 &&
        now_ms - request_entry.enqueue_mono_ms > payload->request.queue_timeout_ms) {
      reply.status = StatusCode::QueueTimeout;
      WriteResponse(request_entry, reply);
      return;
    }

    if (payload->request.payload_size > session.header()->max_request_bytes) {
      reply.status = StatusCode::ProtocolMismatch;
      WriteResponse(request_entry, reply);
      return;
    }

    RpcServerCall call;
    call.opcode = static_cast<Opcode>(request_entry.opcode);
    call.priority = payload->request.priority == static_cast<uint32_t>(Priority::High)
                        ? Priority::High
                        : Priority::Normal;
    call.queue_timeout_ms = payload->request.queue_timeout_ms;
    call.exec_timeout_ms = payload->request.exec_timeout_ms;
    call.flags = payload->request.flags;
    call.payload = PayloadView(request_bytes, payload->request.payload_size);

    const auto it = handlers.find(request_entry.opcode);
    if (it == handlers.end()) {
      reply.status = StatusCode::InvalidArgument;
      WriteResponse(request_entry, reply);
      return;
    }

    const auto start = std::chrono::steady_clock::now();
    it->second(call, &reply);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (payload->request.exec_timeout_ms > 0 &&
        elapsed > static_cast<long long>(payload->request.exec_timeout_ms)) {
      // 当前模型不强杀 handler，只在完成后把结果折叠为 ExecTimeout。
      reply.status = StatusCode::ExecTimeout;
    }
    WriteResponse(request_entry, reply);
  }

  bool DrainQueue(QueueKind kind, WorkerPool* pool) {
    bool drained = false;
    RequestRingEntry entry;
    bool request_ring_became_not_full = false;
    while (pool->HasCapacity() && session.PopRequest(kind, &entry)) {
      const RingCursor& cursor =
          kind == QueueKind::HighRequest ? session.header()->high_ring : session.header()->normal_ring;
      if (cursor.capacity != 0 && RingCount(cursor) + 1u == cursor.capacity) {
        request_ring_became_not_full = true;
      }
      drained = true;
      pool->Enqueue(entry);
    }
    if (request_ring_became_not_full) {
      const uint64_t signal_value = 1;
      write(session.handles().req_credit_event_fd, &signal_value, sizeof(signal_value));
    }
    return drained;
  }

  void DispatcherLoop() {
    pollfd fds[2] = {
        {session.handles().high_req_event_fd, POLLIN, 0},
        {session.handles().normal_req_event_fd, POLLIN, 0},
    };

    while (running.load()) {
      // 有界本地队列可能让共享 request ring 暂时残留任务；当 worker 释放容量后，
      // 下一轮 dispatcher 需要主动回看 shared ring，而不只依赖新的 eventfd 信号。
      bool high_work = DrainQueue(QueueKind::HighRequest, &high_pool);
      if (!high_work) {
        DrainQueue(QueueKind::NormalRequest, &normal_pool);
      }

      const bool high_backlogged = session.header() != nullptr &&
                                   RingCount(session.header()->high_ring) > 0 &&
                                   !high_pool.HasCapacity();
      const bool normal_backlogged = session.header() != nullptr &&
                                     RingCount(session.header()->normal_ring) > 0 &&
                                     !normal_pool.HasCapacity();
      if (high_backlogged) {
        high_pool.WaitForCapacity(std::chrono::milliseconds(100));
        continue;
      }
      if (!high_backlogged && normal_backlogged) {
        normal_pool.WaitForCapacity(std::chrono::milliseconds(100));
        continue;
      }

      const int poll_result = poll(fds, 2, 100);
      uint64_t counter = 0;
      if (poll_result > 0 && (fds[0].revents & POLLIN) != 0) {
        while (read(fds[0].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        high_work = DrainQueue(QueueKind::HighRequest, &high_pool);
      }
      if (poll_result > 0 && !high_work && (fds[1].revents & POLLIN) != 0) {
        // 高优队列一旦有活，当前轮次优先让它排空，再处理普通队列。
        while (read(fds[1].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        DrainQueue(QueueKind::NormalRequest, &normal_pool);
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
  impl_->high_pool.Start(std::max(1u, impl_->options.high_worker_threads),
                         [this](const RequestRingEntry& entry) {
                           impl_->ProcessEntry(entry);
                         });
  impl_->normal_pool.Start(std::max(1u, impl_->options.normal_worker_threads),
                           [this](const RequestRingEntry& entry) {
                             impl_->ProcessEntry(entry);
                           });
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
  impl_->high_pool.Stop();
  impl_->normal_pool.Stop();
  impl_->StopResponseWriter();
  impl_->session.Reset();
}

}  // namespace memrpc
