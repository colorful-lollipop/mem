#include "memrpc/client/rpc_client.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

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

bool AdmissionTimedOut(std::chrono::steady_clock::time_point deadline) {
  return std::chrono::steady_clock::now() >= deadline;
}

bool RingCountIsOneAfterPush(const RingCursor& cursor) {
  const uint32_t tail = cursor.tail.load(std::memory_order_acquire);
  const uint32_t head = cursor.head.load(std::memory_order_acquire);
  return tail - head == 1u;
}

int64_t RemainingTimeoutMs(std::chrono::steady_clock::time_point deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
          .count();
  return remaining > 0 ? remaining : 0;
}

bool ResponseRingBecameNotFull(const SharedMemoryHeader* header) {
  if (header == nullptr || header->response_ring.capacity == 0) {
    return false;
  }
  return RingCount(header->response_ring) + 1u == header->response_ring.capacity;
}

void SignalEventFdIfNeeded(int fd, bool should_signal) {
  if (!should_signal || fd < 0) {
    return;
  }
  const uint64_t signal_value = 1;
  write(fd, &signal_value, sizeof(signal_value));
}

}  // namespace

struct RpcFuture::State {
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool abandoned = false;
  RpcReply reply;
};

RpcFuture::RpcFuture() = default;

RpcFuture::RpcFuture(std::shared_ptr<State> state) : state_(std::move(state)) {}

RpcFuture::~RpcFuture() = default;

bool RpcFuture::IsReady() const {
  if (state_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ready;
}

StatusCode RpcFuture::Wait(RpcReply* reply) {
  if (state_ == nullptr || reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->cv.wait(lock, [this] { return state_->ready; });
  *reply = state_->reply;
  return reply->status;
}

StatusCode RpcFuture::WaitAndTake(RpcReply* reply) {
  if (state_ == nullptr || reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->cv.wait(lock, [this] { return state_->ready; });
  *reply = std::move(state_->reply);
  return reply->status;
}

StatusCode RpcFuture::WaitFor(RpcReply* reply, std::chrono::milliseconds timeout) {
  if (state_ == nullptr || reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->cv.wait_for(lock, timeout, [this] { return state_->ready; })) {
    reply->status = StatusCode::QueueTimeout;
    return reply->status;
  }
  *reply = state_->reply;
  return reply->status;
}

struct RpcClient::Impl {
  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap_channel)
      : bootstrap(std::move(bootstrap_channel)) {}

  struct PendingSubmit {
    RpcCall call;
    uint64_t request_id = 0;
    uint64_t session_id = 0;
    std::shared_ptr<RpcFuture::State> future;
  };

  std::shared_ptr<IBootstrapChannel> bootstrap;
  Session session;
  std::unique_ptr<SlotPool> slot_pool;
  std::mutex reconnect_mutex;
  std::mutex session_mutex;
  std::vector<std::shared_ptr<RpcFuture::State>> pending_slots;
  std::atomic<uint32_t> pending_count{0};
  std::mutex submit_mutex;
  std::condition_variable submit_cv;
  std::deque<PendingSubmit> submit_queue;
  std::thread submit_thread;
  std::atomic<bool> submit_running{false};
  std::atomic<bool> submitter_waiting_for_credit{false};
  std::thread dispatcher_thread;
  std::atomic<bool> dispatcher_running{false};
  std::atomic<bool> shutting_down{false};
  std::atomic<uint64_t> next_request_id{1};
  std::mutex event_mutex;
  RpcEventCallback event_callback;
  std::atomic<uint64_t> current_session_id{0};
  std::atomic<bool> session_dead{true};

  RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(std::move(state));
  }

  void ResolveFuture(const std::shared_ptr<RpcFuture::State>& pending, StatusCode status) {
    if (pending == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(pending->mutex);
    if (pending->ready) {
      return;
    }
    pending->reply.status = status;
    pending->ready = true;
    pending->cv.notify_one();
  }

  void FailQueuedSubmissions(StatusCode status) {
    std::deque<PendingSubmit> queued;
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      queued.swap(submit_queue);
    }
    for (auto& pending : queued) {
      ResolveFuture(pending.future, status);
    }
  }

  void StopSubmitter() {
    submit_running.store(false);
    SignalEventFdIfNeeded(session.handles().req_credit_event_fd, true);
    submit_cv.notify_all();
    if (submit_thread.joinable() && std::this_thread::get_id() != submit_thread.get_id()) {
      submit_thread.join();
    }
  }

  void StartSubmitter() {
    if (submit_running.exchange(true)) {
      return;
    }
    submit_thread = std::thread([this] { SubmitLoop(); });
  }

  void StopDispatcher() {
    dispatcher_running.store(false);
    if (dispatcher_thread.joinable() &&
        std::this_thread::get_id() != dispatcher_thread.get_id()) {
      dispatcher_thread.join();
    }
  }

  void StartDispatcher() {
    dispatcher_running.store(true);
    dispatcher_thread = std::thread([this] { ResponseLoop(); });
  }

  void FailAllPending(StatusCode status_code) {
    for (auto& entry : pending_slots) {
      auto pending = std::move(entry);
      entry.reset();
      ResolveFuture(pending, status_code);
    }
    pending_count.store(0, std::memory_order_relaxed);
  }

  void HandleEngineDeath(uint64_t dead_session_id) {
    if (shutting_down.load()) {
      return;
    }
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (current_session_id.load(std::memory_order_relaxed) == 0 ||
          dead_session_id != current_session_id.load(std::memory_order_relaxed)) {
        return;
      }
    }
    HLOGW("session died, session_id=%{public}llu",
          static_cast<unsigned long long>(dead_session_id));
    // 先停掉响应线程，再整体废弃 session/slot，最后统一失败所有 pending future。
    StopDispatcher();
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      session_dead = true;
      current_session_id = 0;
      session.Reset();
      slot_pool.reset();
    }
    FailQueuedSubmissions(StatusCode::PeerDisconnected);
    FailAllPending(StatusCode::PeerDisconnected);
  }

  StatusCode EnsureLiveSession() {
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (!session_dead && session.valid() && slot_pool != nullptr) {
        return StatusCode::Ok;
      }
    }

    if (bootstrap == nullptr) {
      return StatusCode::InvalidArgument;
    }
    const StatusCode start_status = bootstrap->StartEngine();
    if (start_status != StatusCode::Ok) {
      HLOGE("StartEngine failed, status=%{public}d", static_cast<int>(start_status));
      return start_status;
    }

    BootstrapHandles handles;
    const StatusCode connect_status = bootstrap->Connect(&handles);
    if (connect_status != StatusCode::Ok) {
      HLOGE("Connect failed, status=%{public}d", static_cast<int>(connect_status));
      return connect_status;
    }

    StopDispatcher();
    std::lock_guard<std::mutex> lock(session_mutex);
    // 每次重连都完整替换 session 视图，避免旧 fd / shm 映射残留。
    session_dead = true;
    current_session_id = 0;
    session.Reset();
    slot_pool.reset();

    const StatusCode attach_status = session.Attach(handles);
    if (attach_status != StatusCode::Ok) {
      HLOGE("Session attach failed, status=%{public}d", static_cast<int>(attach_status));
      return attach_status;
    }
    slot_pool = std::make_unique<SlotPool>(session.header()->slot_count,
                                           session.header()->high_reserved_request_slots);
    pending_slots.assign(session.header()->slot_count, nullptr);
    current_session_id = handles.session_id;
    session_dead = false;
    StartDispatcher();
    return StatusCode::Ok;
  }

  bool SubmissionBelongsToDeadSession(const PendingSubmit& pending_submit) {
    if (pending_submit.session_id == 0) {
      return false;
    }
    return session_dead.load(std::memory_order_acquire) ||
           current_session_id.load(std::memory_order_relaxed) == 0 ||
           current_session_id.load(std::memory_order_relaxed) != pending_submit.session_id;
  }

  bool WaitForRequestCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.handles().req_credit_event_fd;
    if (fd < 0) {
      return false;
    }
    pollfd poll_fd{fd, POLLIN, 0};
    submitter_waiting_for_credit.store(true);
    while (submit_running.load() && !shutting_down.load()) {
      const int64_t remaining_ms = RemainingTimeoutMs(deadline);
      if (remaining_ms <= 0) {
        submitter_waiting_for_credit.store(false);
        return false;
      }
      const int poll_result =
          poll(&poll_fd, 1, static_cast<int>(std::min<int64_t>(remaining_ms, 100)));
      if (!submit_running.load() || shutting_down.load()) {
        submitter_waiting_for_credit.store(false);
        return false;
      }
      if (poll_result <= 0) {
        continue;
      }
      if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        submitter_waiting_for_credit.store(false);
        return false;
      }
      if ((poll_fd.revents & POLLIN) == 0) {
        continue;
      }
      uint64_t counter = 0;
      bool drained = false;
      while (read(fd, &counter, sizeof(counter)) == sizeof(counter)) {
        drained = true;
      }
      if (drained) {
        submitter_waiting_for_credit.store(false);
        return true;
      }
    }
    submitter_waiting_for_credit.store(false);
    return false;
  }

  void SubmitLoop() {
    while (submit_running.load()) {
      PendingSubmit pending_submit;
      {
        std::unique_lock<std::mutex> lock(submit_mutex);
        submit_cv.wait(lock, [this] {
          return !submit_running.load(std::memory_order_relaxed) || !submit_queue.empty();
        });
        if (!submit_running.load() && submit_queue.empty()) {
          break;
        }
        pending_submit = std::move(submit_queue.front());
        submit_queue.pop_front();
      }
      SubmitOne(pending_submit);
    }
    FailQueuedSubmissions(StatusCode::PeerDisconnected);
  }

  void SubmitOne(const PendingSubmit& pending_submit) {
    const bool infinite_admission_wait = pending_submit.call.admission_timeout_ms == 0;
    const auto admission_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(pending_submit.call.admission_timeout_ms);

    while (submit_running.load() && !shutting_down.load()) {
      if (SubmissionBelongsToDeadSession(pending_submit)) {
        ResolveFuture(pending_submit.future, StatusCode::PeerDisconnected);
        return;
      }

      const StatusCode ensure_status = EnsureLiveSession();
      if (ensure_status != StatusCode::Ok) {
        ResolveFuture(pending_submit.future, ensure_status);
        return;
      }

      if (SubmissionBelongsToDeadSession(pending_submit)) {
        ResolveFuture(pending_submit.future, StatusCode::PeerDisconnected);
        return;
      }

      bool should_retry_admission = false;
      bool should_wait_for_request_credit = false;
      StatusCode retry_status = StatusCode::QueueFull;

      // Fast-path session liveness check (lock-free).
      if (session_dead.load(std::memory_order_acquire)) {
        should_retry_admission = true;
      } else {
        // Reserve slot (lock-free Treiber stack).
        const auto slot = slot_pool != nullptr
                              ? slot_pool->Reserve(pending_submit.call.priority)
                              : std::optional<uint32_t>{};
        if (!slot.has_value()) {
          should_retry_admission = true;
          should_wait_for_request_credit = (slot_pool != nullptr);
        } else {
          // Brief lock to validate session and get payload pointers.
          SlotPayload* payload = nullptr;
          uint8_t* request_payload = nullptr;
          {
            std::lock_guard<std::mutex> session_lock(session_mutex);
            if (session_dead.load(std::memory_order_relaxed) || !session.valid()) {
              slot_pool->Release(*slot);
              should_retry_admission = true;
            } else {
              payload = session.slot_payload(*slot);
              request_payload = session.slot_request_bytes(*slot);
            }
          }
          if (should_retry_admission) {
            // session died between checks; retry outer loop
          } else if (payload == nullptr || request_payload == nullptr) {
            slot_pool->Release(*slot);
            ResolveFuture(pending_submit.future, StatusCode::EngineInternalError);
            return;
          } else {
            // Everything below runs without session_mutex.
            std::memset(payload, 0, sizeof(SlotPayload));
            payload->runtime.request_id = pending_submit.request_id;
            payload->runtime.state = SlotRuntimeStateCode::Admitted;
            payload->request.queue_timeout_ms = pending_submit.call.queue_timeout_ms;
            payload->request.exec_timeout_ms = pending_submit.call.exec_timeout_ms;
            payload->request.flags = pending_submit.call.flags;
            payload->request.priority = static_cast<uint32_t>(pending_submit.call.priority);
            payload->request.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
            payload->request.payload_size =
                static_cast<uint32_t>(pending_submit.call.payload.size());
            if (!pending_submit.call.payload.empty()) {
              std::memcpy(request_payload, pending_submit.call.payload.data(),
                          pending_submit.call.payload.size());
            }

            RequestRingEntry entry;
            entry.request_id = pending_submit.request_id;
            entry.slot_index = *slot;
            entry.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
            entry.flags = static_cast<uint16_t>(pending_submit.call.flags);
            entry.enqueue_mono_ms = MonotonicNowMs();
            entry.payload_size = payload->request.payload_size;
            payload->runtime.enqueue_mono_ms = entry.enqueue_mono_ms;
            payload->runtime.last_heartbeat_mono_ms = entry.enqueue_mono_ms;
            payload->runtime.state = SlotRuntimeStateCode::Queued;

            pending_slots[*slot] = pending_submit.future;
            pending_count.fetch_add(1, std::memory_order_relaxed);

            const bool high_priority = pending_submit.call.priority == Priority::High;
            slot_pool->Transition(
                *slot, high_priority ? SlotState::QueuedHigh : SlotState::QueuedNormal);
            const StatusCode queue_status =
                session.PushRequest(high_priority ? QueueKind::HighRequest : QueueKind::NormalRequest,
                                    entry);
            if (queue_status != StatusCode::Ok) {
              pending_slots[*slot].reset();
              pending_count.fetch_sub(1, std::memory_order_relaxed);
              slot_pool->Release(*slot);
              if (queue_status == StatusCode::QueueFull) {
                should_retry_admission = true;
                should_wait_for_request_credit = true;
                retry_status = queue_status;
              } else if (queue_status == StatusCode::PeerDisconnected) {
                should_retry_admission = true;
              } else {
                ResolveFuture(pending_submit.future, queue_status);
                return;
              }
            } else {
              const RingCursor& cursor =
                  high_priority ? session.header()->high_ring : session.header()->normal_ring;
              if (RingCountIsOneAfterPush(cursor)) {
                const uint64_t signal_value = 1;
                const int req_fd = high_priority ? session.handles().high_req_event_fd
                                                 : session.handles().normal_req_event_fd;
                if (write(req_fd, &signal_value, sizeof(signal_value)) != sizeof(signal_value)) {
                  pending_slots[*slot].reset();
                  pending_count.fetch_sub(1, std::memory_order_relaxed);
                  slot_pool->Release(*slot);
                  ResolveFuture(pending_submit.future, StatusCode::PeerDisconnected);
                }
              }
              return;
            }
          }
        }
      }

      if (!should_retry_admission) {
        ResolveFuture(pending_submit.future, StatusCode::PeerDisconnected);
        return;
      }
      if (!infinite_admission_wait && AdmissionTimedOut(admission_deadline)) {
        ResolveFuture(pending_submit.future, StatusCode::QueueTimeout);
        return;
      }
      if (should_wait_for_request_credit && !WaitForRequestCredit(admission_deadline)) {
        if (!infinite_admission_wait) {
          ResolveFuture(pending_submit.future, StatusCode::QueueTimeout);
        } else {
          ResolveFuture(pending_submit.future, retry_status);
        }
        return;
      }
    }

    ResolveFuture(pending_submit.future, StatusCode::PeerDisconnected);
  }

  void CompleteRequest(const ResponseRingEntry& entry, bool response_ring_became_not_full) {
    if (slot_pool == nullptr) {
      return;
    }

    SharedSlotPool response_slot_pool(session.response_slot_pool_region());
    RpcReply reply;
    reply.status = static_cast<StatusCode>(entry.status_code);
    reply.engine_code = entry.engine_errno;
    reply.detail_code = entry.detail_code;
    ResponseSlotPayload* response_slot = session.response_slot_payload(entry.slot_index);
    uint8_t* response_bytes = session.response_slot_bytes(entry.slot_index);

    // Look up pending future by request slot index from the response slot.
    uint32_t request_slot_idx = UINT32_MAX;
    std::shared_ptr<RpcFuture::State> pending;
    if (response_slot != nullptr) {
      request_slot_idx = response_slot->response.request_slot_index;
      if (request_slot_idx < pending_slots.size()) {
        pending = std::move(pending_slots[request_slot_idx]);
        pending_slots[request_slot_idx].reset();
        if (pending != nullptr) {
          pending_count.fetch_sub(1, std::memory_order_relaxed);
        }
      }
    }

    if (response_slot != nullptr && response_slot->runtime.request_id != entry.request_id) {
      reply.status = StatusCode::ProtocolMismatch;
      if (pending != nullptr) {
        ResolveFuture(pending, reply.status);
      }
      session.SetState(Session::SessionState::Broken);
      HandleEngineDeath(current_session_id);
      return;
    }
    if (session.header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.result_size > session.header()->max_response_bytes) {
      reply.status = StatusCode::ProtocolMismatch;
    } else {
      reply.payload.assign(response_bytes, response_bytes + entry.result_size);
    }
    if (response_slot != nullptr) {
      response_slot->runtime.state = SlotRuntimeStateCode::Consumed;
      response_slot->runtime.last_update_mono_ms = MonotonicNowMs();
    }
    if (pending != nullptr) {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (!pending->abandoned) {
        pending->reply = std::move(reply);
        pending->ready = true;
        pending->cv.notify_one();
      }
    }
    if (response_slot != nullptr) {
      SlotPayload* request_slot = session.slot_payload(response_slot->response.request_slot_index);
      if (request_slot != nullptr) {
        std::memset(&request_slot->runtime, 0, sizeof(request_slot->runtime));
      }
      const bool request_slot_became_available = slot_pool->available() == 0;
      slot_pool->Release(response_slot->response.request_slot_index);
      SignalEventFdIfNeeded(session.handles().req_credit_event_fd,
                            request_slot_became_available &&
                                submitter_waiting_for_credit.load(std::memory_order_relaxed));
      std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    }
    const bool response_slot_became_available = response_slot_pool.available() == 0;
    response_slot_pool.Release(entry.slot_index);
    SignalEventFdIfNeeded(session.handles().resp_credit_event_fd,
                          response_ring_became_not_full || response_slot_became_available);
  }

  void DeliverEvent(const ResponseRingEntry& entry, bool response_ring_became_not_full) {
    SharedSlotPool response_slot_pool(session.response_slot_pool_region());
    RpcEvent event;
    event.event_domain = entry.event_domain;
    event.event_type = entry.event_type;
    event.flags = entry.flags;
    ResponseSlotPayload* response_slot = session.response_slot_payload(entry.slot_index);
    uint8_t* response_bytes = session.response_slot_bytes(entry.slot_index);
    if (response_slot != nullptr && response_slot->runtime.request_id != entry.request_id) {
      HLOGW("drop mismatched event request_id, expected=%{public}llu slot=%{public}llu",
            static_cast<unsigned long long>(entry.request_id),
            static_cast<unsigned long long>(response_slot->runtime.request_id));
      session.SetState(Session::SessionState::Broken);
      HandleEngineDeath(current_session_id);
      return;
    }
    if (session.header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.result_size > session.header()->max_response_bytes) {
      HLOGW("drop invalid event, size=%{public}u", entry.result_size);
      const bool response_slot_became_available = response_slot_pool.available() == 0;
      response_slot_pool.Release(entry.slot_index);
      SignalEventFdIfNeeded(session.handles().resp_credit_event_fd,
                            response_ring_became_not_full || response_slot_became_available);
      return;
    }
    event.payload.assign(response_bytes, response_bytes + entry.result_size);

    RpcEventCallback callback;
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      callback = event_callback;
    }
    if (callback) {
      callback(event);
    }
    response_slot->runtime.state = SlotRuntimeStateCode::Consumed;
    response_slot->runtime.last_update_mono_ms = MonotonicNowMs();
    std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    const bool response_slot_became_available = response_slot_pool.available() == 0;
    response_slot_pool.Release(entry.slot_index);
    SignalEventFdIfNeeded(session.handles().resp_credit_event_fd,
                          response_ring_became_not_full || response_slot_became_available);
  }

  void ResponseLoop() {
    const int resp_fd = session.handles().resp_event_fd;
    if (resp_fd < 0) {
      return;
    }
    constexpr int kSpinIterations = 256;
    pollfd fd{resp_fd, POLLIN, 0};
    while (dispatcher_running.load()) {
      if (session.state() == Session::SessionState::Broken) {
        HandleEngineDeath(current_session_id);
        return;
      }

      // Try to drain the response ring directly (no syscall).
      ResponseRingEntry entry;
      bool drained = false;
      while (session.PopResponse(&entry)) {
        drained = true;
        const bool response_ring_became_not_full = ResponseRingBecameNotFull(session.header());
        if (entry.message_kind == ResponseMessageKind::Event) {
          DeliverEvent(entry, response_ring_became_not_full);
          continue;
        }
        CompleteRequest(entry, response_ring_became_not_full);
      }
      if (drained) {
        if (session.state() == Session::SessionState::Broken) {
          HandleEngineDeath(current_session_id);
          return;
        }
        continue;
      }

      // Ring was empty. Adaptive spin before falling back to poll.
      bool spin_hit = false;
      for (int i = 0; i < kSpinIterations; ++i) {
        if (RingCount(session.header()->response_ring) > 0) {
          spin_hit = true;
          break;
        }
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
      }
      if (spin_hit) {
        continue;
      }

      // Fall back to poll (with timeout to detect broken sessions).
      const int poll_result = poll(&fd, 1, 100);
      if (poll_result <= 0) {
        continue;
      }
      if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        HandleEngineDeath(current_session_id);
        return;
      }
      if ((fd.revents & POLLIN) != 0) {
        uint64_t counter = 0;
        while (read(fd.fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
      }
    }
  }

  RpcFuture InvokeAsync(RpcCall call) {
    StartSubmitter();
    const StatusCode ensure_status = EnsureLiveSession();
    if (ensure_status != StatusCode::Ok) {
      return this->MakeReadyFuture(ensure_status);
    }

    {
      std::lock_guard<std::mutex> session_lock(session_mutex);
      if (session.header() != nullptr && call.payload.size() > session.header()->max_request_bytes) {
        return this->MakeReadyFuture(StatusCode::InvalidArgument);
      }
    }

    auto pending = std::make_shared<RpcFuture::State>();
    PendingSubmit submit;
    submit.call = std::move(call);
    submit.request_id = next_request_id.fetch_add(1);
    submit.future = pending;
    submit.session_id = current_session_id.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      submit_queue.push_back(std::move(submit));
    }
    submit_cv.notify_one();
    return RpcFuture(std::move(pending));
  }
};

RpcClient::RpcClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : impl_(std::make_unique<Impl>(std::move(bootstrap))) {}

RpcClient::~RpcClient() {
  Shutdown();
}

void RpcClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  impl_->bootstrap = std::move(bootstrap);
}

void RpcClient::SetEventCallback(RpcEventCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->event_mutex);
  impl_->event_callback = std::move(callback);
}

StatusCode RpcClient::Init() {
  if (impl_->bootstrap == nullptr) {
    return StatusCode::InvalidArgument;
  }
  impl_->shutting_down.store(false);
  impl_->bootstrap->SetEngineDeathCallback(
      [this](uint64_t session_id) { impl_->HandleEngineDeath(session_id); });
  impl_->StartSubmitter();
  return impl_->EnsureLiveSession();
}

RpcFuture RpcClient::InvokeAsync(const RpcCall& call) {
  return impl_->InvokeAsync(call);
}

RpcFuture RpcClient::InvokeAsync(RpcCall&& call) {
  return impl_->InvokeAsync(std::move(call));
}

StatusCode RpcClient::InvokeSync(const RpcCall& call, RpcReply* reply) {
  if (reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  RpcFuture future = InvokeAsync(call);
  if (call.admission_timeout_ms == 0 && call.queue_timeout_ms == 0 && call.exec_timeout_ms == 0) {
    return future.Wait(reply);
  }
  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(call.admission_timeout_ms) +
      static_cast<int64_t>(call.queue_timeout_ms) + static_cast<int64_t>(call.exec_timeout_ms));
  return future.WaitFor(reply, wait_budget);
}

RpcClientRuntimeStats RpcClient::GetRuntimeStats() const {
  RpcClientRuntimeStats stats;
  if (impl_ == nullptr) {
    return stats;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->submit_mutex);
    stats.queued_submissions = static_cast<uint32_t>(impl_->submit_queue.size());
  }
  stats.pending_calls = impl_->pending_count.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    if (impl_->slot_pool != nullptr) {
      stats.request_slot_capacity = impl_->slot_pool->capacity();
    }
    if (impl_->session.header() != nullptr) {
      stats.high_request_ring_pending = RingCount(impl_->session.header()->high_ring);
      stats.normal_request_ring_pending = RingCount(impl_->session.header()->normal_ring);
      stats.response_ring_pending = RingCount(impl_->session.header()->response_ring);
    }
  }
  stats.waiting_for_request_credit =
      impl_->submitter_waiting_for_credit.load(std::memory_order_relaxed);
  return stats;
}

void RpcClient::Shutdown() {
  impl_->shutting_down.store(true);
  if (impl_->bootstrap != nullptr) {
    impl_->bootstrap->SetEngineDeathCallback({});
  }
  impl_->StopSubmitter();
  impl_->StopDispatcher();
  {
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    impl_->session_dead = true;
    impl_->current_session_id = 0;
    impl_->session.Reset();
    impl_->slot_pool.reset();
  }
  impl_->FailAllPending(StatusCode::PeerDisconnected);
}

}  // namespace memrpc
