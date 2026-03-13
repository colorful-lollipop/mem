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
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

#include "core/session.h"
#include "core/slot_pool.h"
#include "client/replay_classifier.h"
#include "virus_protection_service_log.h"

namespace memrpc {

#ifndef MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS
#define MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS 50
#endif

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
  const auto now = std::chrono::steady_clock::now();
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return INT64_MAX;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
          .count();
  return remaining > 0 ? remaining : 0;
}

bool ResponseRingBecameNotFull(const SharedMemoryHeader* header) {
  if (header == nullptr || header->responseRing.capacity == 0) {
    return false;
  }
  return RingCount(header->responseRing) + 1u == header->responseRing.capacity;
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
  std::function<void(RpcReply)> callback;
  RpcThenExecutor executor;
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

void RpcFuture::Then(std::function<void(RpcReply)> callback, RpcThenExecutor executor) {
  if (state_ == nullptr || !callback) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->ready) {
    RpcReply reply = std::move(state_->reply);
    lock.unlock();
    if (executor) {
      executor([cb = std::move(callback), r = std::move(reply)]() mutable {
        cb(std::move(r));
      });
    } else {
      callback(std::move(reply));
    }
    return;
  }
  state_->callback = std::move(callback);
  state_->executor = std::move(executor);
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

  struct PendingInfo {
    Opcode opcode = OPCODE_INVALID;
    Priority priority = Priority::Normal;
    uint32_t flags = 0;
    uint32_t admission_timeout_ms = 0;
    uint32_t queue_timeout_ms = 0;
    uint32_t exec_timeout_ms = 0;
    uint64_t request_id = 0;
    uint64_t session_id = 0;
    ReplayHint replay_hint = ReplayHint::Unknown;
    RpcRuntimeState last_runtime_state = RpcRuntimeState::Unknown;
  };

  std::shared_ptr<IBootstrapChannel> bootstrap;
  Session session;
  std::unique_ptr<SlotPool> slot_pool;
  std::mutex reconnect_mutex;
  std::mutex session_mutex;
  std::vector<std::shared_ptr<RpcFuture::State>> pending_slots;
  std::vector<std::optional<PendingInfo>> pending_info_slots;
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
  std::mutex policy_mutex;
  RecoveryPolicy recovery_policy;
  std::atomic<uint64_t> current_session_id{0};
  std::atomic<bool> session_dead{true};
  std::atomic<bool> session_live{false};
  std::thread watchdog_thread;
  std::atomic<bool> watchdog_running{false};
  std::atomic<uint32_t> last_activity_mono_ms{0};
  uint32_t last_idle_notify_mono_ms{0};
  std::thread restart_thread;
  std::atomic<bool> restart_pending{false};
  std::atomic<bool> suppress_death_callback{false};

  RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(std::move(state));
  }

  void TouchActivity() {
    last_activity_mono_ms.store(MonotonicNowMs(), std::memory_order_relaxed);
  }

  PendingInfo MakePendingInfo(const PendingSubmit& submit) {
    PendingInfo info;
    info.opcode = submit.call.opcode;
    info.priority = submit.call.priority;
    info.flags = submit.call.flags;
    info.admission_timeout_ms = submit.call.admission_timeout_ms;
    info.queue_timeout_ms = submit.call.queue_timeout_ms;
    info.exec_timeout_ms = submit.call.exec_timeout_ms;
    info.request_id = submit.request_id;
    info.session_id = submit.session_id;
    return info;
  }

  void NotifyFailure(const PendingInfo& info, StatusCode status, FailureStage stage) {
    if (status == StatusCode::Ok) {
      return;
    }
    std::function<RecoveryDecision(const RpcFailure&)> on_failure;
    {
      std::lock_guard<std::mutex> lock(policy_mutex);
      on_failure = recovery_policy.onFailure;
    }
    if (!on_failure) {
      return;
    }
    RpcFailure failure;
    failure.status = status;
    failure.opcode = info.opcode;
    failure.priority = info.priority;
    failure.flags = info.flags;
    failure.admission_timeout_ms = info.admission_timeout_ms;
    failure.queue_timeout_ms = info.queue_timeout_ms;
    failure.exec_timeout_ms = info.exec_timeout_ms;
    failure.request_id = info.request_id;
    failure.session_id = info.session_id;
    failure.monotonic_ms = MonotonicNowMs();
    failure.stage = stage;
    failure.replay_hint = info.replay_hint;
    failure.last_runtime_state = info.last_runtime_state;
    RecoveryDecision decision = on_failure(failure);
    if (decision.action == RecoveryAction::Restart) {
      RequestForcedRestart(decision.delay_ms);
    }
  }

  void FailAndResolve(const PendingInfo& info, StatusCode status, FailureStage stage,
                      const std::shared_ptr<RpcFuture::State>& future) {
    NotifyFailure(info, status, stage);
    ResolveFuture(future, status);
  }

  void ResolveFuture(const std::shared_ptr<RpcFuture::State>& pending, StatusCode status) {
    if (pending == nullptr) {
      return;
    }
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (pending->ready) {
      return;
    }
    pending->reply.status = status;
    pending->ready = true;
    if (pending->callback) {
      auto cb = std::move(pending->callback);
      auto exec = std::move(pending->executor);
      RpcReply reply = std::move(pending->reply);
      lock.unlock();
      if (exec) {
        exec([cb = std::move(cb), reply = std::move(reply)]() mutable {
          cb(std::move(reply));
        });
      } else {
        cb(std::move(reply));
      }
    } else {
      pending->cv.notify_one();
    }
  }

  void FailQueuedSubmissions(StatusCode status) {
    std::deque<PendingSubmit> queued;
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      queued.swap(submit_queue);
    }
    for (auto& pending : queued) {
      NotifyFailure(MakePendingInfo(pending), status, FailureStage::Session);
      ResolveFuture(pending.future, status);
    }
  }

  void StopSubmitter() {
    submit_running.store(false);
    SignalEventFdIfNeeded(session.Handles().reqCreditEventFd, true);
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

  void StopWatchdog() {
    watchdog_running.store(false);
    if (watchdog_thread.joinable() &&
        std::this_thread::get_id() != watchdog_thread.get_id()) {
      watchdog_thread.join();
    }
  }

  void StartWatchdog() {
    if (watchdog_running.exchange(true)) {
      return;
    }
    watchdog_thread = std::thread([this] { WatchdogLoop(); });
  }

  void WatchdogLoop() {
    constexpr uint32_t WATCHDOG_INTERVAL_MS = MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS;
    while (watchdog_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_INTERVAL_MS));
      if (!watchdog_running.load() || shutting_down.load()) {
        break;
      }

      // --- Async timeout scanning ---
      if (!session_dead.load(std::memory_order_acquire) && pending_count.load() > 0) {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (!session_dead.load(std::memory_order_relaxed) && session.valid()) {
          ScanPendingTimeouts();
        }
      }

      // --- Idle reminder ---
      uint32_t cur_idle_timeout_ms = 0;
      uint32_t cur_idle_notify_interval_ms = 0;
      {
        std::lock_guard<std::mutex> lock(policy_mutex);
        cur_idle_timeout_ms = recovery_policy.idle_timeout_ms;
        cur_idle_notify_interval_ms = recovery_policy.idle_notify_interval_ms;
      }
      if (cur_idle_timeout_ms > 0 && cur_idle_notify_interval_ms > 0) {
        const uint32_t now_ms = MonotonicNowMs();
        const uint32_t last_ms = last_activity_mono_ms.load(std::memory_order_relaxed);
        if (last_ms > 0 && now_ms - last_ms >= cur_idle_timeout_ms) {
          const uint32_t idle_ms = now_ms - last_ms;
          if (last_idle_notify_mono_ms == 0 ||
              now_ms - last_idle_notify_mono_ms >= cur_idle_notify_interval_ms) {
            last_idle_notify_mono_ms = now_ms;
            std::function<RecoveryDecision(uint64_t)> on_idle;
            {
              std::lock_guard<std::mutex> lock(policy_mutex);
              on_idle = recovery_policy.onIdle;
            }
            if (on_idle) {
              RecoveryDecision decision = on_idle(idle_ms);
              if (decision.action == RecoveryAction::Restart) {
                RequestForcedRestart(decision.delay_ms);
              }
            }
          }
        } else {
          last_idle_notify_mono_ms = 0;
        }
      }
    }
  }

  struct TimeoutCheckResult {
    bool timed_out = false;
    StatusCode status = StatusCode::ExecTimeout;
  };

  TimeoutCheckResult CheckSlotTimeout(const SlotRuntimeState& rt, const PendingInfo& info,
                                      uint32_t now_ms) {
    TimeoutCheckResult result;
    switch (rt.state) {
      case SlotRuntimeStateCode::Admitted:
      case SlotRuntimeStateCode::Queued:
        if (info.queue_timeout_ms > 0 && rt.enqueue_mono_ms > 0 &&
            now_ms - rt.enqueue_mono_ms >= info.queue_timeout_ms) {
          result.timed_out = true;
          result.status = StatusCode::QueueTimeout;
        }
        break;
      case SlotRuntimeStateCode::Executing:
      case SlotRuntimeStateCode::Responding: {
        if (info.exec_timeout_ms > 0) {
          const uint32_t ref_ms =
              rt.start_exec_mono_ms > 0 ? rt.start_exec_mono_ms : rt.enqueue_mono_ms;
          if (ref_ms > 0 && now_ms - ref_ms >= info.exec_timeout_ms) {
            result.timed_out = true;
            result.status = StatusCode::ExecTimeout;
          }
        }
        break;
      }
      default:
        break;
    }
    return result;
  }

  void ExpireTimedOutSlot(size_t slot_index, StatusCode status, const SlotRuntimeState& rt) {
    PendingInfo timeout_info = *pending_info_slots[slot_index];
    timeout_info.last_runtime_state = ToRpcRuntimeState(rt.state);
    timeout_info.replay_hint = ClassifyReplayHint(rt.state);
    auto pending = std::move(pending_slots[slot_index]);
    pending_slots[slot_index].reset();
    pending_info_slots[slot_index].reset();
    pending_count.fetch_sub(1, std::memory_order_relaxed);
    FailAndResolve(timeout_info, status, FailureStage::Timeout, pending);
    if (slot_pool != nullptr) {
      SlotPayload* mutable_payload = session.slotPayload(static_cast<uint32_t>(slot_index));
      if (mutable_payload != nullptr) {
        std::memset(&mutable_payload->runtime, 0, sizeof(mutable_payload->runtime));
      }
      slot_pool->Release(static_cast<uint32_t>(slot_index));
    }
  }

  void ScanPendingTimeouts() {
    const uint32_t now_ms = MonotonicNowMs();
    for (size_t i = 0; i < pending_slots.size(); ++i) {
      if (pending_slots[i] == nullptr || !pending_info_slots[i].has_value()) {
        continue;
      }
      const SlotPayload* payload = session.slotPayload(static_cast<uint32_t>(i));
      if (payload == nullptr) {
        continue;
      }
      const auto check = CheckSlotTimeout(payload->runtime, *pending_info_slots[i], now_ms);
      if (check.timed_out) {
        ExpireTimedOutSlot(i, check.status, payload->runtime);
      }
    }
  }

  void FailAllPending(StatusCode status_code) {
    for (size_t i = 0; i < pending_slots.size(); ++i) {
      auto pending = std::move(pending_slots[i]);
      pending_slots[i].reset();
      if (pending != nullptr && i < pending_info_slots.size() && pending_info_slots[i].has_value()) {
        NotifyFailure(*pending_info_slots[i], status_code, FailureStage::Session);
        pending_info_slots[i].reset();
      }
      ResolveFuture(pending, status_code);  // NOLINT(FailAndResolve not used here: info may not exist)
    }
    pending_count.store(0, std::memory_order_relaxed);
  }

  struct ReplayableSnapshot {
    std::vector<PendingSubmit> replay_list;
    struct PoisonEntry {
      PendingInfo info;
      std::shared_ptr<RpcFuture::State> future;
    };
    std::vector<PoisonEntry> poison_list;
  };

  // Must be called under session_mutex, before session.Reset().
  // Separates pending requests into replay-safe and poison-pill categories.
  ReplayableSnapshot SaveReplayableRequests() {
    ReplayableSnapshot snapshot;

    // Classify pending slots (already in shared memory).
    for (size_t i = 0; i < pending_slots.size(); ++i) {
      if (pending_slots[i] == nullptr || !pending_info_slots[i].has_value()) {
        continue;
      }
      const PendingInfo& info = *pending_info_slots[i];
      if (info.replay_hint == ReplayHint::SafeToReplay) {
        PendingSubmit ps;
        ps.call.opcode = info.opcode;
        ps.call.priority = info.priority;
        ps.call.flags = info.flags;
        ps.call.admission_timeout_ms = info.admission_timeout_ms;
        ps.call.queue_timeout_ms = info.queue_timeout_ms;
        ps.call.exec_timeout_ms = info.exec_timeout_ms;
        // Recover payload from shared memory.
        const SlotPayload* payload = session.slotPayload(static_cast<uint32_t>(i));
        const uint8_t* request_bytes = session.slotRequestBytes(static_cast<uint32_t>(i));
        if (payload != nullptr && request_bytes != nullptr && payload->request.payload_size > 0) {
          ps.call.payload.assign(request_bytes, request_bytes + payload->request.payload_size);
        }
        ps.request_id = info.request_id;
        ps.session_id = info.session_id;
        ps.future = std::move(pending_slots[i]);
        snapshot.replay_list.push_back(std::move(ps));
      } else {
        snapshot.poison_list.push_back({info, std::move(pending_slots[i])});
      }
      pending_slots[i].reset();
      pending_info_slots[i].reset();
    }
    pending_count.store(0, std::memory_order_relaxed);

    // All queued submissions are SafeToReplay (not yet written to shm).
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      for (auto& qs : submit_queue) {
        snapshot.replay_list.push_back(std::move(qs));
      }
      submit_queue.clear();
    }

    return snapshot;
  }

  void FailSavedRequests(std::vector<PendingSubmit>& saved, StatusCode status) {
    for (auto& item : saved) {
      ResolveFuture(item.future, status);
    }
    saved.clear();
  }

  void RestartAfterDeath(uint32_t delay_ms,
                         std::vector<PendingSubmit> saved_requests) {
    // Segmented sleep, checking shutting_down every 100ms.
    for (uint32_t elapsed = 0; elapsed < delay_ms && !shutting_down.load(); elapsed += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min(100u, delay_ms - elapsed)));
    }
    if (shutting_down.load()) {
      FailSavedRequests(saved_requests, StatusCode::PeerDisconnected);
      return;
    }

    StatusCode status = EnsureLiveSession();
    if (status != StatusCode::Ok) {
      FailSavedRequests(saved_requests, StatusCode::PeerDisconnected);
      return;
    }

    // Inject replay requests at the front of submit_queue.
    uint64_t new_session_id = current_session_id.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      for (auto& item : saved_requests) {
        item.session_id = new_session_id;
        item.request_id = next_request_id.fetch_add(1);
      }
      submit_queue.insert(submit_queue.begin(),
          std::make_move_iterator(saved_requests.begin()),
          std::make_move_iterator(saved_requests.end()));
    }
    submit_cv.notify_one();
  }

  void RequestForcedRestart(uint32_t delay_ms) {
    if (restart_pending.exchange(true)) {
      return;  // Another restart already in progress.
    }
    if (shutting_down.load()) {
      restart_pending.store(false);
      return;
    }
    // Snapshot replayable requests and tear down the session.
    StopDispatcher();
    ReplayableSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      for (size_t i = 0; i < pending_info_slots.size(); ++i) {
        if (!pending_info_slots[i].has_value() || pending_slots[i] == nullptr) {
          continue;
        }
        const SlotPayload* payload = session.slotPayload(static_cast<uint32_t>(i));
        if (payload != nullptr) {
          pending_info_slots[i]->last_runtime_state = ToRpcRuntimeState(payload->runtime.state);
          pending_info_slots[i]->replay_hint = ClassifyReplayHint(payload->runtime.state);
        }
      }
      snapshot = SaveReplayableRequests();
      session_live.store(false, std::memory_order_release);
      session_dead = true;
      current_session_id = 0;
      session.Reset();
      slot_pool.reset();
    }

    // Fail poison-pill futures.
    for (auto& pe : snapshot.poison_list) {
      NotifyFailure(pe.info, StatusCode::CrashedDuringExecution, FailureStage::Session);
      ResolveFuture(pe.future, StatusCode::CrashedDuringExecution);
    }

    // Close the existing session (suppress death callback to avoid re-entry).
    suppress_death_callback.store(true);
    if (bootstrap != nullptr) {
      bootstrap->CloseSession();
    }
    suppress_death_callback.store(false);

    // Spawn restart thread.
    if (restart_thread.joinable()) {
      restart_thread.join();
    }
    restart_thread = std::thread([this, delay = delay_ms,
                                  saved = std::move(snapshot.replay_list)]() mutable {
      RestartAfterDeath(delay, std::move(saved));
      restart_pending.store(false);
    });
  }

  void HandleEngineDeath(uint64_t dead_session_id) {
    if (shutting_down.load()) {
      return;
    }
    if (suppress_death_callback.load()) {
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
    HILOGW("session died, session_id=%{public}llu",
          static_cast<unsigned long long>(dead_session_id));
    StopDispatcher();

    // Check policy presence early.
    std::function<RecoveryDecision(const EngineDeathReport&)> on_engine_death;
    {
      std::lock_guard<std::mutex> lock(policy_mutex);
      on_engine_death = recovery_policy.onEngineDeath;
    }

    if (!on_engine_death) {
      // No handler — legacy behavior: fail everything.
      {
        std::lock_guard<std::mutex> lock(session_mutex);
        for (size_t i = 0; i < pending_info_slots.size(); ++i) {
          if (!pending_info_slots[i].has_value() || pending_slots[i] == nullptr) {
            continue;
          }
          const SlotPayload* payload = session.slotPayload(static_cast<uint32_t>(i));
          if (payload != nullptr) {
            pending_info_slots[i]->last_runtime_state = ToRpcRuntimeState(payload->runtime.state);
            pending_info_slots[i]->replay_hint = ClassifyReplayHint(payload->runtime.state);
          }
        }
        session_live.store(false, std::memory_order_release);
        session_dead = true;
        current_session_id = 0;
        session.Reset();
        slot_pool.reset();
      }
      FailQueuedSubmissions(StatusCode::PeerDisconnected);
      FailAllPending(StatusCode::PeerDisconnected);
      return;
    }

    // Handler exists — snapshot, classify, and tear down session.
    ReplayableSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      for (size_t i = 0; i < pending_info_slots.size(); ++i) {
        if (!pending_info_slots[i].has_value() || pending_slots[i] == nullptr) {
          continue;
        }
        const SlotPayload* payload = session.slotPayload(static_cast<uint32_t>(i));
        if (payload != nullptr) {
          pending_info_slots[i]->last_runtime_state = ToRpcRuntimeState(payload->runtime.state);
          pending_info_slots[i]->replay_hint = ClassifyReplayHint(payload->runtime.state);
        }
      }
      snapshot = SaveReplayableRequests();
      session_live.store(false, std::memory_order_release);
      session_dead = true;
      current_session_id = 0;
      session.Reset();
      slot_pool.reset();
    }

    // Build report.
    EngineDeathReport report;
    report.dead_session_id = dead_session_id;
    report.safe_to_replay_count = static_cast<uint32_t>(snapshot.replay_list.size());
    for (const auto& pe : snapshot.poison_list) {
      EngineDeathReport::PoisonPillSuspect suspect;
      suspect.request_id = pe.info.request_id;
      suspect.opcode = pe.info.opcode;
      suspect.last_state = pe.info.last_runtime_state;
      report.poison_pill_suspects.push_back(suspect);
    }

    // Call handler.
    RecoveryDecision decision = on_engine_death(report);

    // Fail poison-pill futures with CrashedDuringExecution.
    for (auto& pe : snapshot.poison_list) {
      NotifyFailure(pe.info, StatusCode::CrashedDuringExecution, FailureStage::Session);
      ResolveFuture(pe.future, StatusCode::CrashedDuringExecution);
    }

    if (decision.action == RecoveryAction::Ignore) {
      // Fail all replay-safe futures too.
      for (auto& ps : snapshot.replay_list) {
        PendingInfo info = MakePendingInfo(ps);
        NotifyFailure(info, StatusCode::PeerDisconnected, FailureStage::Session);
        ResolveFuture(ps.future, StatusCode::PeerDisconnected);
      }
      return;
    }

    // Restart: join any prior restart thread, then spawn a new one.
    if (restart_thread.joinable()) {
      restart_thread.join();
    }
    restart_thread = std::thread([this, delay = decision.delay_ms,
                                  saved = std::move(snapshot.replay_list)]() mutable {
      RestartAfterDeath(delay, std::move(saved));
    });
  }

  StatusCode EnsureLiveSession() {
    if (session_live.load(std::memory_order_acquire)) {
      return StatusCode::Ok;
    }
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (session_live.load(std::memory_order_relaxed)) {
        return StatusCode::Ok;
      }
    }

    if (bootstrap == nullptr) {
      return StatusCode::InvalidArgument;
    }

    BootstrapHandles handles;
    const StatusCode open_status = bootstrap->OpenSession(handles);
    if (open_status != StatusCode::Ok) {
      HILOGE("OpenSession failed, status=%{public}d", static_cast<int>(open_status));
      return open_status;
    }

    StopDispatcher();
    std::lock_guard<std::mutex> lock(session_mutex);
    // 每次重连都完整替换 session 视图，避免旧 fd / shm 映射残留。
    session_live.store(false, std::memory_order_release);
    session_dead = true;
    current_session_id = 0;
    session.Reset();
    slot_pool.reset();

    const StatusCode attach_status = session.Attach(handles);
    if (attach_status != StatusCode::Ok) {
      HILOGE("Session attach failed, status=%{public}d", static_cast<int>(attach_status));
      return attach_status;
    }
    slot_pool = std::make_unique<SlotPool>(session.Header()->slotCount);
    pending_slots.assign(session.Header()->slotCount, nullptr);
    pending_info_slots.assign(session.Header()->slotCount, std::nullopt);
    current_session_id = handles.session_id;
    session_dead = false;
    session_live.store(true, std::memory_order_release);
    TouchActivity();
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
    const int fd = session.Handles().reqCreditEventFd;
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

  struct SlotReservation {
    std::optional<uint32_t> slot;
    SlotPayload* payload = nullptr;
    uint8_t* request_bytes = nullptr;
    bool should_retry = false;
    bool should_wait_credit = false;
  };

  SlotReservation ReserveSlotAndPayload() {
    SlotReservation result;
    if (session_dead.load(std::memory_order_acquire)) {
      result.should_retry = true;
      return result;
    }
    result.slot = slot_pool != nullptr ? slot_pool->Reserve() : std::optional<uint32_t>{};
    if (!result.slot.has_value()) {
      result.should_retry = true;
      result.should_wait_credit = (slot_pool != nullptr);
      return result;
    }
    std::lock_guard<std::mutex> session_lock(session_mutex);
    if (session_dead.load(std::memory_order_relaxed) || !session.valid()) {
      slot_pool->Release(*result.slot);
      result.slot.reset();
      result.should_retry = true;
      return result;
    }
    result.payload = session.slotPayload(*result.slot);
    result.request_bytes = session.slotRequestBytes(*result.slot);
    return result;
  }

  void FillSlotPayload(SlotPayload* payload, uint8_t* request_bytes,
                        const PendingSubmit& pending_submit) {
    std::memset(payload, 0, sizeof(SlotPayload));
    payload->runtime.request_id = pending_submit.request_id;
    payload->runtime.state = SlotRuntimeStateCode::Admitted;
    payload->request.queue_timeout_ms = pending_submit.call.queue_timeout_ms;
    payload->request.exec_timeout_ms = pending_submit.call.exec_timeout_ms;
    payload->request.flags = pending_submit.call.flags;
    payload->request.priority = static_cast<uint32_t>(pending_submit.call.priority);
    payload->request.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
    payload->request.payload_size = static_cast<uint32_t>(pending_submit.call.payload.size());
    if (!pending_submit.call.payload.empty()) {
      std::memcpy(request_bytes, pending_submit.call.payload.data(),
                  pending_submit.call.payload.size());
    }
  }

  RequestRingEntry BuildRingEntry(const PendingSubmit& pending_submit, uint32_t slot,
                                   SlotPayload* payload) {
    RequestRingEntry entry;
    entry.request_id = pending_submit.request_id;
    entry.slot_index = slot;
    entry.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
    entry.flags = static_cast<uint16_t>(pending_submit.call.flags);
    entry.enqueue_mono_ms = MonotonicNowMs();
    entry.payload_size = payload->request.payload_size;
    payload->runtime.enqueue_mono_ms = entry.enqueue_mono_ms;
    payload->runtime.last_heartbeat_mono_ms = entry.enqueue_mono_ms;
    payload->runtime.state = SlotRuntimeStateCode::Queued;
    return entry;
  }

  enum class PushOutcome { Success, RetryWithCredit, RetryWithoutCredit, FatalError };

  PushOutcome PushAndSignalRequest(const RequestRingEntry& entry, uint32_t slot,
                                    const PendingSubmit& pending_submit, const PendingInfo& info) {
    const bool high_priority = pending_submit.call.priority == Priority::High;
    slot_pool->Transition(slot, high_priority ? SlotState::QueuedHigh : SlotState::QueuedNormal);
    const StatusCode queue_status =
        session.PushRequest(high_priority ? QueueKind::HighRequest : QueueKind::NormalRequest,
                            entry);
    if (queue_status != StatusCode::Ok) {
      pending_slots[slot].reset();
      pending_info_slots[slot].reset();
      pending_count.fetch_sub(1, std::memory_order_relaxed);
      slot_pool->Release(slot);
      if (queue_status == StatusCode::QueueFull) {
        return PushOutcome::RetryWithCredit;
      }
      if (queue_status == StatusCode::PeerDisconnected) {
        return PushOutcome::RetryWithoutCredit;
      }
      FailAndResolve(info, queue_status, FailureStage::Admission, pending_submit.future);
      return PushOutcome::FatalError;
    }
    const RingCursor& cursor =
        high_priority ? session.Header()->highRing : session.Header()->normalRing;
    if (RingCountIsOneAfterPush(cursor)) {
      const uint64_t signal_value = 1;
      const int req_fd = high_priority ? session.Handles().highReqEventFd
                                       : session.Handles().normalReqEventFd;
      if (write(req_fd, &signal_value, sizeof(signal_value)) != sizeof(signal_value)) {
        pending_slots[slot].reset();
        pending_info_slots[slot].reset();
        pending_count.fetch_sub(1, std::memory_order_relaxed);
        slot_pool->Release(slot);
        FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission,
                       pending_submit.future);
        return PushOutcome::FatalError;
      }
    }
    TouchActivity();
    return PushOutcome::Success;
  }

  enum class SubmitAttemptResult { Done, Retry, RetryWithCredit };

  SubmitAttemptResult TrySubmitOnce(const PendingSubmit& pending_submit, const PendingInfo& info) {
    auto reservation = ReserveSlotAndPayload();
    if (!reservation.should_retry && reservation.slot.has_value()) {
      if (reservation.payload == nullptr || reservation.request_bytes == nullptr) {
        slot_pool->Release(*reservation.slot);
        FailAndResolve(info, StatusCode::EngineInternalError, FailureStage::Admission,
                       pending_submit.future);
        return SubmitAttemptResult::Done;
      }
      FillSlotPayload(reservation.payload, reservation.request_bytes, pending_submit);
      auto entry = BuildRingEntry(pending_submit, *reservation.slot, reservation.payload);
      pending_slots[*reservation.slot] = pending_submit.future;
      pending_info_slots[*reservation.slot] = info;
      pending_count.fetch_add(1, std::memory_order_relaxed);
      const auto outcome =
          PushAndSignalRequest(entry, *reservation.slot, pending_submit, info);
      if (outcome == PushOutcome::Success || outcome == PushOutcome::FatalError) {
        return SubmitAttemptResult::Done;
      }
      reservation.should_retry = true;
      reservation.should_wait_credit = (outcome == PushOutcome::RetryWithCredit);
    }
    if (!reservation.should_retry) {
      FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission,
                     pending_submit.future);
      return SubmitAttemptResult::Done;
    }
    return reservation.should_wait_credit ? SubmitAttemptResult::RetryWithCredit
                                          : SubmitAttemptResult::Retry;
  }

  void SubmitOne(const PendingSubmit& pending_submit) {
    const bool infinite_admission_wait = pending_submit.call.admission_timeout_ms == 0;
    const auto admission_deadline = infinite_admission_wait
                                    ? std::chrono::steady_clock::time_point::max()
                                    : std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(pending_submit.call.admission_timeout_ms);
    const PendingInfo info = MakePendingInfo(pending_submit);

    while (submit_running.load() && !shutting_down.load()) {
      if (SubmissionBelongsToDeadSession(pending_submit)) {
        FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission,
                       pending_submit.future);
        return;
      }
      const StatusCode ensure_status = EnsureLiveSession();
      if (ensure_status != StatusCode::Ok) {
        FailAndResolve(info, ensure_status, FailureStage::Admission, pending_submit.future);
        return;
      }
      if (SubmissionBelongsToDeadSession(pending_submit)) {
        FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission,
                       pending_submit.future);
        return;
      }

      const auto attempt = TrySubmitOnce(pending_submit, info);
      if (attempt == SubmitAttemptResult::Done) {
        return;
      }
      if (!infinite_admission_wait && AdmissionTimedOut(admission_deadline)) {
        FailAndResolve(info, StatusCode::QueueTimeout, FailureStage::Admission,
                       pending_submit.future);
        return;
      }
      if (attempt == SubmitAttemptResult::RetryWithCredit &&
          !WaitForRequestCredit(admission_deadline)) {
        const StatusCode fail_status =
            infinite_admission_wait ? StatusCode::QueueFull : StatusCode::QueueTimeout;
        FailAndResolve(info, fail_status, FailureStage::Admission, pending_submit.future);
        return;
      }
    }
    FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission,
                   pending_submit.future);
  }

  struct PendingLookup {
    uint32_t request_slot_idx = UINT32_MAX;
    std::shared_ptr<RpcFuture::State> future;
    std::optional<PendingInfo> info;
  };

  PendingLookup LookupPendingFuture(const ResponseSlotPayload* response_slot) {
    PendingLookup result;
    if (response_slot == nullptr) {
      return result;
    }
    result.request_slot_idx = response_slot->response.request_slot_index;
    if (result.request_slot_idx >= pending_slots.size()) {
      return result;
    }
    result.future = std::move(pending_slots[result.request_slot_idx]);
    pending_slots[result.request_slot_idx].reset();
    if (result.request_slot_idx < pending_info_slots.size()) {
      result.info = std::move(pending_info_slots[result.request_slot_idx]);
      pending_info_slots[result.request_slot_idx].reset();
    }
    if (result.future != nullptr) {
      pending_count.fetch_sub(1, std::memory_order_relaxed);
    }
    return result;
  }

  void ResolveCompletedFuture(const std::shared_ptr<RpcFuture::State>& pending, RpcReply reply) {
    if (pending == nullptr) {
      return;
    }
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (pending->abandoned) {
      return;
    }
    pending->reply = std::move(reply);
    pending->ready = true;
    if (pending->callback) {
      auto cb = std::move(pending->callback);
      auto exec = std::move(pending->executor);
      RpcReply cb_reply = std::move(pending->reply);
      lock.unlock();
      if (exec) {
        exec([cb = std::move(cb), cb_reply = std::move(cb_reply)]() mutable {
          cb(std::move(cb_reply));
        });
      } else {
        cb(std::move(cb_reply));
      }
    } else {
      pending->cv.notify_one();
    }
  }

  void CleanupCompletedSlots(const ResponseSlotPayload* response_slot,
                             const std::shared_ptr<RpcFuture::State>& pending,
                             uint32_t response_slot_index,
                             bool responseRing_became_not_full) {
    SharedSlotPool response_slot_pool(session.responseSlotPoolRegion());
    if (response_slot != nullptr && pending != nullptr) {
      SlotPayload* request_slot = session.slotPayload(response_slot->response.request_slot_index);
      if (request_slot != nullptr) {
        std::memset(&request_slot->runtime, 0, sizeof(request_slot->runtime));
      }
      const bool request_slot_became_available = slot_pool->available() == 0;
      slot_pool->Release(response_slot->response.request_slot_index);
      SignalEventFdIfNeeded(session.Handles().reqCreditEventFd,
                            request_slot_became_available &&
                                submitter_waiting_for_credit.load(std::memory_order_relaxed));
    }
    if (response_slot != nullptr) {
      std::memset(const_cast<ResponseSlotPayload*>(response_slot), 0, sizeof(ResponseSlotPayload));
    }
    const bool response_slot_became_available = response_slot_pool.available() == 0;
    response_slot_pool.Release(response_slot_index);
    SignalEventFdIfNeeded(session.Handles().respCreditEventFd,
                          responseRing_became_not_full || response_slot_became_available);
  }

  void CompleteRequest(const ResponseRingEntry& entry, bool responseRing_became_not_full) {
    if (slot_pool == nullptr) {
      return;
    }

    RpcReply reply;
    reply.status = static_cast<StatusCode>(entry.statusCode);
    reply.engine_code = entry.engineErrno;
    reply.detail_code = entry.detailCode;
    ResponseSlotPayload* response_slot = session.responseSlotPayload(entry.slotIndex);
    uint8_t* response_bytes = session.responseSlotBytes(entry.slotIndex);

    auto lookup = LookupPendingFuture(response_slot);

    if (response_slot != nullptr && response_slot->runtime.request_id != entry.requestId) {
      reply.status = StatusCode::ProtocolMismatch;
      if (lookup.info.has_value()) {
        NotifyFailure(*lookup.info, reply.status, FailureStage::Response);
      }
      ResolveFuture(lookup.future, reply.status);
      session.SetState(Session::SessionState::Broken);
      HandleEngineDeath(current_session_id);
      return;
    }
    if (session.Header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.resultSize > session.Header()->maxResponseBytes) {
      reply.status = StatusCode::ProtocolMismatch;
    } else {
      reply.payload.assign(response_bytes, response_bytes + entry.resultSize);
    }
    if (response_slot != nullptr) {
      response_slot->runtime.state = SlotRuntimeStateCode::Consumed;
      response_slot->runtime.last_update_mono_ms = MonotonicNowMs();
    }
    if (reply.status != StatusCode::Ok && lookup.info.has_value()) {
      NotifyFailure(*lookup.info, reply.status, FailureStage::Response);
    }
    ResolveCompletedFuture(lookup.future, std::move(reply));
    if (lookup.future != nullptr) {
      TouchActivity();
    }
    CleanupCompletedSlots(response_slot, lookup.future, entry.slotIndex,
                          responseRing_became_not_full);
  }

  void DeliverEvent(const ResponseRingEntry& entry, bool responseRing_became_not_full) {
    SharedSlotPool response_slot_pool(session.responseSlotPoolRegion());
    RpcEvent event;
    event.event_domain = entry.eventDomain;
    event.event_type = entry.eventType;
    event.flags = entry.flags;
    ResponseSlotPayload* response_slot = session.responseSlotPayload(entry.slotIndex);
    uint8_t* response_bytes = session.responseSlotBytes(entry.slotIndex);
    if (response_slot != nullptr && response_slot->runtime.request_id != entry.requestId) {
      HILOGW("drop mismatched event request_id, expected=%{public}llu slot=%{public}llu",
            static_cast<unsigned long long>(entry.requestId),
            static_cast<unsigned long long>(response_slot->runtime.request_id));
      session.SetState(Session::SessionState::Broken);
      HandleEngineDeath(current_session_id);
      return;
    }
    if (session.Header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.resultSize > session.Header()->maxResponseBytes) {
      HILOGW("drop invalid event, size=%{public}u", entry.resultSize);
      const bool response_slot_became_available = response_slot_pool.available() == 0;
      response_slot_pool.Release(entry.slotIndex);
      SignalEventFdIfNeeded(session.Handles().respCreditEventFd,
                            responseRing_became_not_full || response_slot_became_available);
      return;
    }
    event.payload.assign(response_bytes, response_bytes + entry.resultSize);

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
    response_slot_pool.Release(entry.slotIndex);
    SignalEventFdIfNeeded(session.Handles().respCreditEventFd,
                          responseRing_became_not_full || response_slot_became_available);
  }

  bool DrainResponseRing() {
    ResponseRingEntry entry;
    bool drained = false;
    while (session.PopResponse(&entry)) {
      drained = true;
      const bool responseRing_became_not_full = ResponseRingBecameNotFull(session.Header());
      if (entry.messageKind == ResponseMessageKind::Event) {
        DeliverEvent(entry, responseRing_became_not_full);
        continue;
      }
      CompleteRequest(entry, responseRing_became_not_full);
    }
    return drained;
  }

  bool SpinForResponseRing(int iterations) {
    for (int i = 0; i < iterations; ++i) {
      if (RingCount(session.Header()->responseRing) > 0) {
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

  bool PollAndDrainEventFd(pollfd* fd) {
    const int poll_result = poll(fd, 1, 100);
    if (poll_result <= 0) {
      return true;
    }
    if ((fd->revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      HandleEngineDeath(current_session_id);
      return false;
    }
    if ((fd->revents & POLLIN) != 0) {
      uint64_t counter = 0;
      while (read(fd->fd, &counter, sizeof(counter)) == sizeof(counter)) {
      }
    }
    return true;
  }

  void ResponseLoop() {
    const int resp_fd = session.Handles().respEventFd;
    if (resp_fd < 0) {
      return;
    }
    constexpr int SPIN_ITERATIONS = 256;
    pollfd fd{resp_fd, POLLIN, 0};
    while (dispatcher_running.load()) {
      if (session.State() == Session::SessionState::Broken) {
        HandleEngineDeath(current_session_id);
        return;
      }
      if (DrainResponseRing()) {
        if (session.State() == Session::SessionState::Broken) {
          HandleEngineDeath(current_session_id);
          return;
        }
        continue;
      }
      if (SpinForResponseRing(SPIN_ITERATIONS)) {
        continue;
      }
      if (!PollAndDrainEventFd(&fd)) {
        return;
      }
    }
  }

  RpcFuture InvokeAsync(RpcCall call) {
    StartSubmitter();
    const uint64_t request_id = next_request_id.fetch_add(1);
    const StatusCode ensure_status = EnsureLiveSession();
    if (ensure_status != StatusCode::Ok) {
      PendingInfo info;
      info.opcode = call.opcode;
      info.priority = call.priority;
      info.flags = call.flags;
      info.admission_timeout_ms = call.admission_timeout_ms;
      info.queue_timeout_ms = call.queue_timeout_ms;
      info.exec_timeout_ms = call.exec_timeout_ms;
      info.request_id = request_id;
      NotifyFailure(info, ensure_status, FailureStage::Admission);
      return this->MakeReadyFuture(ensure_status);
    }

    {
      std::lock_guard<std::mutex> session_lock(session_mutex);
      if (session.Header() != nullptr && call.payload.size() > session.Header()->maxRequestBytes) {
        PendingInfo info;
        info.opcode = call.opcode;
        info.priority = call.priority;
        info.flags = call.flags;
        info.admission_timeout_ms = call.admission_timeout_ms;
        info.queue_timeout_ms = call.queue_timeout_ms;
        info.exec_timeout_ms = call.exec_timeout_ms;
        info.request_id = request_id;
        NotifyFailure(info, StatusCode::InvalidArgument, FailureStage::Admission);
        return this->MakeReadyFuture(StatusCode::InvalidArgument);
      }
    }

    auto pending = std::make_shared<RpcFuture::State>();
    PendingSubmit submit;
    submit.call = std::move(call);
    submit.request_id = request_id;
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

void RpcClient::SetRecoveryPolicy(RecoveryPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->policy_mutex);
  impl_->recovery_policy = std::move(policy);
}

StatusCode RpcClient::Init() {
  if (impl_->bootstrap == nullptr) {
    return StatusCode::InvalidArgument;
  }
  impl_->shutting_down.store(false);
  impl_->bootstrap->SetEngineDeathCallback(
      [this](uint64_t session_id) { impl_->HandleEngineDeath(session_id); });
  impl_->StartSubmitter();
  const StatusCode status = impl_->EnsureLiveSession();
  if (status == StatusCode::Ok) {
    impl_->StartWatchdog();
  }
  return status;
}

RpcFuture RpcClient::InvokeAsync(const RpcCall& call) {
  return impl_->InvokeAsync(call);
}

RpcFuture RpcClient::InvokeAsync(RpcCall&& call) {
  return impl_->InvokeAsync(std::move(call));
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
    if (impl_->session.Header() != nullptr) {
      stats.high_request_ring_pending = RingCount(impl_->session.Header()->highRing);
      stats.normal_request_ring_pending = RingCount(impl_->session.Header()->normalRing);
      stats.response_ring_pending = RingCount(impl_->session.Header()->responseRing);
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
  if (impl_->restart_thread.joinable()) {
    impl_->restart_thread.join();
  }
  impl_->StopSubmitter();
  impl_->StopWatchdog();
  impl_->StopDispatcher();
  {
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    impl_->session_live.store(false, std::memory_order_release);
    impl_->session_dead = true;
    impl_->current_session_id = 0;
    impl_->session.Reset();
    impl_->slot_pool.reset();
  }
  impl_->FailAllPending(StatusCode::PeerDisconnected);
  if (impl_->bootstrap != nullptr) {
    impl_->bootstrap->CloseSession();
  }
}

// --- RpcSyncClient ---

RpcSyncClient::RpcSyncClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap)) {}

RpcSyncClient::~RpcSyncClient() = default;

void RpcSyncClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  client_.SetBootstrapChannel(std::move(bootstrap));
}

void RpcSyncClient::SetEventCallback(RpcEventCallback callback) {
  client_.SetEventCallback(std::move(callback));
}

void RpcSyncClient::SetRecoveryPolicy(RecoveryPolicy policy) {
  client_.SetRecoveryPolicy(std::move(policy));
}

StatusCode RpcSyncClient::Init() {
  return client_.Init();
}

StatusCode RpcSyncClient::InvokeSync(const RpcCall& call, RpcReply* reply) {
  if (reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  RpcFuture future = client_.InvokeAsync(call);
  if (call.admission_timeout_ms == 0 && call.queue_timeout_ms == 0 && call.exec_timeout_ms == 0) {
    return future.Wait(reply);
  }
  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(call.admission_timeout_ms) +
      static_cast<int64_t>(call.queue_timeout_ms) + static_cast<int64_t>(call.exec_timeout_ms));
  return future.WaitFor(reply, wait_budget);
}

RpcClientRuntimeStats RpcSyncClient::GetRuntimeStats() const {
  return client_.GetRuntimeStats();
}

void RpcSyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace memrpc
