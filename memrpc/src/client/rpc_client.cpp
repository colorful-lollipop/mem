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
#include "memrpc/core/runtime_utils.h"
#include "virus_protection_service_log.h"

namespace MemRpc {

#ifndef MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS
#define MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS 50
#endif

namespace {

constexpr size_t MAX_SUBMIT_QUEUE_SIZE = 3000;

bool ResponseRingBecameNotFull(const SharedMemoryHeader* header) {
  if (header == nullptr || header->responseRing.capacity == 0) {
    return false;
  }
  return RingCount(header->responseRing) + 1U == header->responseRing.capacity;
}

void SignalEventFdIfNeeded(int fd, bool should_signal) {
  if (!should_signal || fd < 0) {
    return;
  }
  (void)SignalEventFd(fd);
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

struct RpcClient::Impl {  // NOLINT(clang-analyzer-optin.performance.Padding)
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

  static RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(std::move(state));
  }

  void TouchActivity() {
    last_activity_mono_ms.store(MonotonicNowMs(), std::memory_order_relaxed);
  }

  static PendingInfo MakePendingInfo(const PendingSubmit& submit) {
    PendingInfo info;
    info.opcode = submit.call.opcode;
    info.priority = submit.call.priority;
    info.flags = submit.call.flags;
    info.admission_timeout_ms = submit.call.admissionTimeoutMs;
    info.queue_timeout_ms = submit.call.queueTimeoutMs;
    info.exec_timeout_ms = submit.call.execTimeoutMs;
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
    failure.admissionTimeoutMs = info.admission_timeout_ms;
    failure.queueTimeoutMs = info.queue_timeout_ms;
    failure.execTimeoutMs = info.exec_timeout_ms;
    failure.requestId = info.request_id;
    failure.sessionId = info.session_id;
    failure.monotonicMs = MonotonicNowMs();
    failure.stage = stage;
    failure.replayHint = info.replay_hint;
    failure.lastRuntimeState = info.last_runtime_state;
    RecoveryDecision decision = on_failure(failure);
    if (decision.action == RecoveryAction::Restart) {
      RequestForcedRestart(decision.delayMs);
    }
  }

  void FailAndResolve(const PendingInfo& info, StatusCode status, FailureStage stage,
                      const std::shared_ptr<RpcFuture::State>& future) {
    NotifyFailure(info, status, stage);
    ResolveFuture(future, status);
  }

  static void ResolveFuture(const std::shared_ptr<RpcFuture::State>& pending, StatusCode status) {
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
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      submit_running.store(false, std::memory_order_release);
    }
    SignalEventFdIfNeeded(session.Handles().reqCreditEventFd, true);
    submit_cv.notify_all();
    if (submit_thread.joinable() && std::this_thread::get_id() != submit_thread.get_id()) {
      submit_thread.join();
    }
  }

  void StartSubmitter() {
    if (submit_running.exchange(true, std::memory_order_acq_rel)) {
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

  void ScanPendingTimeoutsIfNeeded() {
    if (session_dead.load(std::memory_order_acquire) || pending_count.load() == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(session_mutex);
    if (!session_dead.load(std::memory_order_relaxed) && session.Valid()) {
      ScanPendingTimeouts();
    }
  }

  void HandleIdlePolicyDecision(uint32_t idle_ms) {
    std::function<RecoveryDecision(uint64_t)> on_idle;
    {
      std::lock_guard<std::mutex> lock(policy_mutex);
      on_idle = recovery_policy.onIdle;
    }
    if (!on_idle) {
      return;
    }
    const RecoveryDecision decision = on_idle(idle_ms);
    if (decision.action == RecoveryAction::Restart) {
      RequestForcedRestart(decision.delayMs);
    }
  }

  void HandleIdleReminder() {
    uint32_t idle_timeout_ms = 0;
    uint32_t idle_notify_interval_ms = 0;
    {
      std::lock_guard<std::mutex> lock(policy_mutex);
      idle_timeout_ms = recovery_policy.idleTimeoutMs;
      idle_notify_interval_ms = recovery_policy.idleNotifyIntervalMs;
    }
    if (idle_timeout_ms == 0 || idle_notify_interval_ms == 0) {
      return;
    }

    const uint32_t now_ms = MonotonicNowMs();
    const uint32_t last_ms = last_activity_mono_ms.load(std::memory_order_relaxed);
    if (last_ms == 0 || now_ms - last_ms < idle_timeout_ms) {
      last_idle_notify_mono_ms = 0;
      return;
    }
    if (last_idle_notify_mono_ms != 0 &&
        now_ms - last_idle_notify_mono_ms < idle_notify_interval_ms) {
      return;
    }
    last_idle_notify_mono_ms = now_ms;
    HandleIdlePolicyDecision(now_ms - last_ms);
  }

  void WatchdogLoop() {
    constexpr uint32_t WATCHDOG_INTERVAL_MS = MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS;
    while (watchdog_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_INTERVAL_MS));
      if (!watchdog_running.load() || shutting_down.load()) {
        break;
      }
      ScanPendingTimeoutsIfNeeded();
      HandleIdleReminder();
    }
  }

  struct TimeoutCheckResult {
    bool timed_out = false;
    StatusCode status = StatusCode::ExecTimeout;
  };

  static bool IsQueueState(SlotRuntimeStateCode state) {
    return state == SlotRuntimeStateCode::Admitted || state == SlotRuntimeStateCode::Queued;
  }

  static bool IsExecutionState(SlotRuntimeStateCode state) {
    return state == SlotRuntimeStateCode::Executing ||
           state == SlotRuntimeStateCode::Responding;
  }

  static bool QueueTimedOut(const SlotRuntimeState& rt, const PendingInfo& info, uint32_t now_ms) {
    return info.queue_timeout_ms > 0 && rt.enqueueMonoMs > 0 &&
           now_ms - rt.enqueueMonoMs >= info.queue_timeout_ms;
  }

  static bool ExecTimedOut(const SlotRuntimeState& rt, const PendingInfo& info, uint32_t now_ms) {
    if (info.exec_timeout_ms == 0) {
      return false;
    }
    const uint32_t ref_ms = rt.startExecMonoMs > 0 ? rt.startExecMonoMs : rt.enqueueMonoMs;
    return ref_ms > 0 && now_ms - ref_ms >= info.exec_timeout_ms;
  }

  static TimeoutCheckResult CheckSlotTimeout(const SlotRuntimeState& rt,
                                             const PendingInfo& info,
                                             uint32_t now_ms) {
    if (IsQueueState(rt.state) && QueueTimedOut(rt, info, now_ms)) {
      return {true, StatusCode::QueueTimeout};
    }
    if (IsExecutionState(rt.state) && ExecTimedOut(rt, info, now_ms)) {
      return {true, StatusCode::ExecTimeout};
    }
    return {};
  }

  void ExpireTimedOutSlot(size_t slotIndex, StatusCode status, const SlotRuntimeState& rt) {
    if (slotIndex >= pending_info_slots.size()) {
      return;
    }
    const std::optional<PendingInfo> timeout_info_opt = pending_info_slots[slotIndex];
    if (!timeout_info_opt.has_value()) {
      return;
    }
    PendingInfo timeout_info = timeout_info_opt.value_or(PendingInfo{});
    timeout_info.last_runtime_state = ToRpcRuntimeState(rt.state);
    timeout_info.replay_hint = ClassifyReplayHint(rt.state);
    auto pending = std::move(pending_slots[slotIndex]);
    pending_slots[slotIndex].reset();
    pending_info_slots[slotIndex].reset();
    pending_count.fetch_sub(1, std::memory_order_relaxed);
    FailAndResolve(timeout_info, status, FailureStage::Timeout, pending);
    if (slot_pool != nullptr) {
      SlotPayload* mutable_payload = session.GetSlotPayload(static_cast<uint32_t>(slotIndex));
      if (mutable_payload != nullptr) {
        std::memset(&mutable_payload->runtime, 0, sizeof(mutable_payload->runtime));
      }
      slot_pool->Release(static_cast<uint32_t>(slotIndex));
    }
  }

  void ScanPendingTimeouts() {
    const uint32_t now_ms = MonotonicNowMs();
    for (size_t i = 0; i < pending_slots.size(); ++i) {
      const auto& pending_info = pending_info_slots[i];
      if (pending_slots[i] == nullptr || !pending_info.has_value()) {
        continue;
      }
      const SlotPayload* payload = session.GetSlotPayload(static_cast<uint32_t>(i));
      if (payload == nullptr) {
        continue;
      }
      const auto check = CheckSlotTimeout(payload->runtime, pending_info.value(), now_ms);
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
        const PendingInfo info = pending_info_slots[i].value_or(PendingInfo{});
        NotifyFailure(info, status_code, FailureStage::Session);
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

  PendingSubmit BuildReplayableSubmit(size_t slot_index, const PendingInfo& info) {
    PendingSubmit submit;
    submit.call.opcode = info.opcode;
    submit.call.priority = info.priority;
    submit.call.flags = info.flags;
    submit.call.admissionTimeoutMs = info.admission_timeout_ms;
    submit.call.queueTimeoutMs = info.queue_timeout_ms;
    submit.call.execTimeoutMs = info.exec_timeout_ms;
    const SlotPayload* payload = session.GetSlotPayload(static_cast<uint32_t>(slot_index));
    const uint8_t* request_bytes = session.GetSlotRequestBytes(static_cast<uint32_t>(slot_index));
    if (payload != nullptr && request_bytes != nullptr && payload->request.payloadSize > 0) {
      submit.call.payload.assign(request_bytes, request_bytes + payload->request.payloadSize);
    }
    submit.request_id = info.request_id;
    submit.session_id = info.session_id;
    return submit;
  }

  void DrainQueuedSubmissionsForReplay(std::vector<PendingSubmit>* replay_list) {
    if (replay_list == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(submit_mutex);
    for (auto& queued_submit : submit_queue) {
      replay_list->push_back(std::move(queued_submit));
    }
    submit_queue.clear();
  }

  // Must be called under session_mutex, before session.Reset().
  // Separates pending requests into replay-safe and poison-pill categories.
  ReplayableSnapshot SaveReplayableRequests() {
    ReplayableSnapshot snapshot;

    for (size_t i = 0; i < pending_slots.size(); ++i) {
      const auto& pending_info = pending_info_slots[i];
      if (pending_slots[i] == nullptr || !pending_info.has_value()) {
        continue;
      }
      const PendingInfo info = pending_info.value_or(PendingInfo{});
      if (info.replay_hint == ReplayHint::SafeToReplay) {
        PendingSubmit ps = BuildReplayableSubmit(i, info);
        ps.future = std::move(pending_slots[i]);
        snapshot.replay_list.push_back(std::move(ps));
      } else {
        snapshot.poison_list.push_back({info, std::move(pending_slots[i])});
      }
      pending_slots[i].reset();
      pending_info_slots[i].reset();
    }
    pending_count.store(0, std::memory_order_relaxed);
    DrainQueuedSubmissionsForReplay(&snapshot.replay_list);
    return snapshot;
  }

  static void FailSavedRequests(std::vector<PendingSubmit>& saved, StatusCode status) {
    for (auto& item : saved) {
      ResolveFuture(item.future, status);
    }
    saved.clear();
  }

  // Must be called under session_mutex.
  void AnnotatePendingRuntimeStates() {
    for (size_t i = 0; i < pending_info_slots.size(); ++i) {
      auto& pending_info = pending_info_slots[i];
      if (!pending_info.has_value() || pending_slots[i] == nullptr) {
        continue;
      }
      const SlotPayload* payload = session.GetSlotPayload(static_cast<uint32_t>(i));
      if (payload != nullptr) {
        PendingInfo& info = pending_info.value();
        info.last_runtime_state = ToRpcRuntimeState(payload->runtime.state);
        info.replay_hint = ClassifyReplayHint(payload->runtime.state);
      }
    }
  }

  // Must be called under session_mutex.
  void ResetSessionState() {
    session_live.store(false, std::memory_order_release);
    session_dead = true;
    current_session_id = 0;
    session.Reset();
    slot_pool.reset();
  }

  // Must be called under session_mutex. Annotates, snapshots, and tears down.
  ReplayableSnapshot SnapshotAndTearDownLocked() {
    AnnotatePendingRuntimeStates();
    ReplayableSnapshot snapshot = SaveReplayableRequests();
    ResetSessionState();
    return snapshot;
  }

  void FailPoisonPills(std::vector<ReplayableSnapshot::PoisonEntry>& poison_list) {
    for (auto& pe : poison_list) {
      NotifyFailure(pe.info, StatusCode::CrashedDuringExecution, FailureStage::Session);
      ResolveFuture(pe.future, StatusCode::CrashedDuringExecution);
    }
  }

  static EngineDeathReport BuildDeathReport(uint64_t dead_session_id,
                                            const ReplayableSnapshot& snapshot) {
    EngineDeathReport report;
    report.deadSessionId = dead_session_id;
    report.safeToReplayCount = static_cast<uint32_t>(snapshot.replay_list.size());
    for (const auto& pe : snapshot.poison_list) {
      EngineDeathReport::PoisonPillSuspect suspect;
      suspect.requestId = pe.info.request_id;
      suspect.opcode = pe.info.opcode;
      suspect.lastState = pe.info.last_runtime_state;
      report.poisonPillSuspects.push_back(suspect);
    }
    return report;
  }

  void HandleDeathNoPolicy() {
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      AnnotatePendingRuntimeStates();
      ResetSessionState();
    }
    FailQueuedSubmissions(StatusCode::PeerDisconnected);
    FailAllPending(StatusCode::PeerDisconnected);
  }

  void FailReplayList(std::vector<PendingSubmit>& replay_list) {
    for (auto& ps : replay_list) {
      PendingInfo info = MakePendingInfo(ps);
      NotifyFailure(info, StatusCode::PeerDisconnected, FailureStage::Session);
      ResolveFuture(ps.future, StatusCode::PeerDisconnected);
    }
  }

  void RestartAfterDeath(uint32_t delay_ms,
                         std::vector<PendingSubmit> saved_requests) {
    // Segmented sleep, checking shutting_down every 100ms.
    for (uint32_t elapsed = 0; elapsed < delay_ms && !shutting_down.load(); elapsed += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min(100U, delay_ms - elapsed)));
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
    StopDispatcher();
    ReplayableSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      snapshot = SnapshotAndTearDownLocked();
    }
    FailPoisonPills(snapshot.poison_list);

    // Close the existing session (suppress death callback to avoid re-entry).
    suppress_death_callback.store(true);
    if (bootstrap != nullptr) {
      bootstrap->CloseSession();
    }
    suppress_death_callback.store(false);

    if (restart_thread.joinable()) {
      restart_thread.join();
    }
    restart_thread = std::thread([this, delay = delay_ms,
                                  saved = std::move(snapshot.replay_list)]() mutable {
      RestartAfterDeath(delay, std::move(saved));
      restart_pending.store(false);
    });
  }

  bool SessionMatchesDeadSession(uint64_t dead_session_id) {
    std::lock_guard<std::mutex> lock(session_mutex);
    const uint64_t current_session = current_session_id.load(std::memory_order_relaxed);
    return current_session != 0 && dead_session_id == current_session;
  }

  std::function<RecoveryDecision(const EngineDeathReport&)> GetEngineDeathPolicy() {
    std::lock_guard<std::mutex> lock(policy_mutex);
    return recovery_policy.onEngineDeath;
  }

  void StartReplayRestartThread(uint32_t delay_ms, std::vector<PendingSubmit> replay_list) {
    if (restart_thread.joinable()) {
      restart_thread.join();
    }
    restart_thread = std::thread([this, delay = delay_ms,
                                  saved = std::move(replay_list)]() mutable {
      RestartAfterDeath(delay, std::move(saved));
    });
  }

  void HandleEngineDeath(uint64_t dead_session_id) {
    if (shutting_down.load() || suppress_death_callback.load()) {
      return;
    }
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    if (!SessionMatchesDeadSession(dead_session_id)) {
      return;
    }
    HILOGW("session died, session_id=%{public}llu",
           static_cast<unsigned long long>(dead_session_id));
    StopDispatcher();

    const auto on_engine_death = GetEngineDeathPolicy();
    if (!on_engine_death) {
      HandleDeathNoPolicy();
      return;
    }

    ReplayableSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      snapshot = SnapshotAndTearDownLocked();
    }

    EngineDeathReport report = BuildDeathReport(dead_session_id, snapshot);
    RecoveryDecision decision = on_engine_death(report);
    FailPoisonPills(snapshot.poison_list);

    if (decision.action == RecoveryAction::Ignore) {
      FailReplayList(snapshot.replay_list);
      return;
    }
    StartReplayRestartThread(decision.delayMs, std::move(snapshot.replay_list));
  }

  [[nodiscard]] bool HasLiveSessionFastPath() const {
    return session_live.load(std::memory_order_acquire);
  }

  bool HasLiveSessionLocked() {
    std::lock_guard<std::mutex> lock(session_mutex);
    return session_live.load(std::memory_order_relaxed);
  }

  StatusCode OpenBootstrapSession(BootstrapHandles* handles) {
    if (bootstrap == nullptr || handles == nullptr) {
      return StatusCode::InvalidArgument;
    }
    const StatusCode open_status = bootstrap->OpenSession(*handles);
    if (open_status != StatusCode::Ok) {
      HILOGE("OpenSession failed, status=%{public}d", static_cast<int>(open_status));
    }
    return open_status;
  }

  StatusCode ReplaceSession(BootstrapHandles handles) {
    StopDispatcher();
    std::lock_guard<std::mutex> lock(session_mutex);
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
    current_session_id = handles.sessionId;
    session_dead = false;
    session_live.store(true, std::memory_order_release);
    return StatusCode::Ok;
  }

  StatusCode EnsureLiveSession() {
    if (HasLiveSessionFastPath()) {
      return StatusCode::Ok;
    }
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    if (HasLiveSessionLocked()) {
      return StatusCode::Ok;
    }

    BootstrapHandles handles;
    const StatusCode open_status = OpenBootstrapSession(&handles);
    if (open_status != StatusCode::Ok) {
      return open_status;
    }
    const StatusCode attach_status = ReplaceSession(handles);
    if (attach_status != StatusCode::Ok) {
      return attach_status;
    }
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

  PollEventFdResult WaitForRequestCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = session.Handles().reqCreditEventFd;
    if (fd < 0) {
      return PollEventFdResult::Failed;
    }
    pollfd poll_fd{fd, POLLIN, 0};
    submitter_waiting_for_credit.store(true);
    [[maybe_unused]] const auto clear_waiting =
        MakeScopeExit([this] { submitter_waiting_for_credit.store(false); });
    while (submit_running.load() && !shutting_down.load()) {
      const int64_t remaining_ms = RemainingTimeoutMs(deadline);
      if (remaining_ms <= 0) {
        return PollEventFdResult::Timeout;
      }
      const auto wait_result =
          PollEventFd(&poll_fd, static_cast<int>(std::min<int64_t>(remaining_ms, 100)));
      if (wait_result == PollEventFdResult::Retry ||
          wait_result == PollEventFdResult::Timeout) {
        continue;
      }
      return wait_result;
    }
    return PollEventFdResult::Failed;
  }

  void SubmitLoop() {
    while (submit_running.load(std::memory_order_acquire)) {
      PendingSubmit pending_submit;
      {
        std::unique_lock<std::mutex> lock(submit_mutex);
        submit_cv.wait(lock, [this] {
          return !submit_running.load(std::memory_order_acquire) || !submit_queue.empty();
        });
        if (!submit_running.load(std::memory_order_acquire) && submit_queue.empty()) {
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
    if (session_dead.load(std::memory_order_relaxed) || !session.Valid()) {
      slot_pool->Release(*result.slot);
      result.slot.reset();
      result.should_retry = true;
      return result;
    }
    result.payload = session.GetSlotPayload(*result.slot);
    result.request_bytes = session.GetSlotRequestBytes(*result.slot);
    return result;
  }

  static void FillSlotPayload(SlotPayload* payload, uint8_t* request_bytes,
                              const PendingSubmit& pending_submit) {
    std::memset(payload, 0, sizeof(SlotPayload));
    payload->runtime.requestId = pending_submit.request_id;
    payload->runtime.state = SlotRuntimeStateCode::Admitted;
    payload->request.queueTimeoutMs = pending_submit.call.queueTimeoutMs;
    payload->request.execTimeoutMs = pending_submit.call.execTimeoutMs;
    payload->request.flags = pending_submit.call.flags;
    payload->request.priority = static_cast<uint32_t>(pending_submit.call.priority);
    payload->request.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
    payload->request.payloadSize = static_cast<uint32_t>(pending_submit.call.payload.size());
    if (!pending_submit.call.payload.empty()) {
      std::memcpy(request_bytes, pending_submit.call.payload.data(),
                  pending_submit.call.payload.size());
    }
  }

  static RequestRingEntry BuildRingEntry(const PendingSubmit& pending_submit,
                                         uint32_t slot,
                                         SlotPayload* payload) {
    RequestRingEntry entry;
    entry.requestId = pending_submit.request_id;
    entry.slotIndex = slot;
    entry.opcode = static_cast<uint16_t>(pending_submit.call.opcode);
    entry.flags = static_cast<uint16_t>(pending_submit.call.flags);
    entry.enqueueMonoMs = MonotonicNowMs();
    entry.payloadSize = payload->request.payloadSize;
    payload->runtime.enqueueMonoMs = entry.enqueueMonoMs;
    payload->runtime.lastHeartbeatMonoMs = entry.enqueueMonoMs;
    payload->runtime.state = SlotRuntimeStateCode::Queued;
    return entry;
  }

  enum class PushOutcome : uint8_t { Success, RetryWithCredit, RetryWithoutCredit, FatalError };

  PushOutcome FailQueuedRequest(uint32_t slot, StatusCode queue_status,
                                const PendingSubmit& pending_submit, const PendingInfo& info) {
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

  [[nodiscard]] bool SignalQueuedRequest(bool high_priority) const {
    const RingCursor& cursor =
        high_priority ? session.Header()->highRing : session.Header()->normalRing;
    if (!RingCountIsOneAfterPush(cursor)) {
      return true;
    }
    const int req_fd = high_priority ? session.Handles().highReqEventFd
                                     : session.Handles().normalReqEventFd;
    return SignalEventFd(req_fd);
  }

  PushOutcome PushAndSignalRequest(const RequestRingEntry& entry, uint32_t slot,
                                   const PendingSubmit& pending_submit,
                                   const PendingInfo& info) {
    const bool high_priority = pending_submit.call.priority == Priority::High;
    slot_pool->Transition(slot, high_priority ? SlotState::QueuedHigh : SlotState::QueuedNormal);
    const StatusCode queue_status =
        session.PushRequest(high_priority ? QueueKind::HighRequest : QueueKind::NormalRequest,
                            entry);
    if (queue_status != StatusCode::Ok) {
      return FailQueuedRequest(slot, queue_status, pending_submit, info);
    }
    if (!SignalQueuedRequest(high_priority)) {
      HILOGW("request published without wakeup signal, request_id=%{public}llu slot=%{public}u",
             static_cast<unsigned long long>(pending_submit.request_id), slot);
    }
    TouchActivity();
    return PushOutcome::Success;
  }

  enum class SubmitAttemptResult : uint8_t { Done, Retry, RetryWithCredit };

  void FailAdmission(const PendingInfo& info, StatusCode status,
                     const std::shared_ptr<RpcFuture::State>& future) {
    FailAndResolve(info, status, FailureStage::Admission, future);
  }

  StatusCode PrepareSubmissionSession(const PendingSubmit& pending_submit) {
    if (SubmissionBelongsToDeadSession(pending_submit)) {
      return StatusCode::PeerDisconnected;
    }
    const StatusCode ensure_status = EnsureLiveSession();
    if (ensure_status != StatusCode::Ok) {
      return ensure_status;
    }
    return SubmissionBelongsToDeadSession(pending_submit)
               ? StatusCode::PeerDisconnected
               : StatusCode::Ok;
  }

  std::optional<StatusCode> WaitForAdmissionRetry(SubmitAttemptResult attempt,
                                                  bool infinite_admission_wait,
                                                  std::chrono::steady_clock::time_point deadline) {
    if (!infinite_admission_wait && DeadlineReached(deadline)) {
      return StatusCode::QueueTimeout;
    }
    if (attempt != SubmitAttemptResult::RetryWithCredit) {
      return std::nullopt;
    }
    switch (WaitForRequestCredit(deadline)) {
      case PollEventFdResult::Ready:
        return std::nullopt;
      case PollEventFdResult::Timeout:
        return infinite_admission_wait ? StatusCode::QueueFull : StatusCode::QueueTimeout;
      case PollEventFdResult::Retry:
        return std::nullopt;
      case PollEventFdResult::Failed:
        return StatusCode::PeerDisconnected;
    }
    return StatusCode::PeerDisconnected;
  }

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
    const bool infinite_admission_wait = pending_submit.call.admissionTimeoutMs == 0;
    const auto admission_deadline = infinite_admission_wait
                                    ? std::chrono::steady_clock::time_point::max()
                                    : std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(pending_submit.call.admissionTimeoutMs);
    const PendingInfo info = MakePendingInfo(pending_submit);

    while (submit_running.load() && !shutting_down.load()) {
      const StatusCode session_status = PrepareSubmissionSession(pending_submit);
      if (session_status != StatusCode::Ok) {
        FailAdmission(info, session_status, pending_submit.future);
        return;
      }

      const auto attempt = TrySubmitOnce(pending_submit, info);
      if (attempt == SubmitAttemptResult::Done) {
        return;
      }
      const auto retry_status =
          WaitForAdmissionRetry(attempt, infinite_admission_wait, admission_deadline);
      if (retry_status.has_value()) {
        FailAdmission(info, *retry_status, pending_submit.future);
        return;
      }
    }
    FailAdmission(info, StatusCode::PeerDisconnected, pending_submit.future);
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
    result.request_slot_idx = response_slot->response.requestSlotIndex;
    if (result.request_slot_idx >= pending_slots.size()) {
      return result;
    }
    result.future = std::move(pending_slots[result.request_slot_idx]);
    pending_slots[result.request_slot_idx].reset();
    if (result.request_slot_idx < pending_info_slots.size()) {
      result.info = pending_info_slots[result.request_slot_idx];
      pending_info_slots[result.request_slot_idx].reset();
    }
    if (result.future != nullptr) {
      pending_count.fetch_sub(1, std::memory_order_relaxed);
    }
    return result;
  }

  static void ResolveCompletedFuture(const std::shared_ptr<RpcFuture::State>& pending,
                                     RpcReply reply) {
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

  bool HandleMismatchedResponse(const ResponseRingEntry& entry, ResponseSlotPayload* response_slot,
                                const PendingLookup& lookup, RpcReply* reply) {
    if (response_slot == nullptr || response_slot->runtime.requestId == entry.requestId) {
      return false;
    }
    reply->status = StatusCode::ProtocolMismatch;
    if (lookup.info.has_value()) {
      NotifyFailure(*lookup.info, reply->status, FailureStage::Response);
    }
    ResolveFuture(lookup.future, reply->status);
    session.SetState(Session::SessionState::Broken);
    HandleEngineDeath(current_session_id);
    return true;
  }

  void FillReplyPayload(const ResponseRingEntry& entry, ResponseSlotPayload* response_slot,
                        uint8_t* response_bytes, RpcReply* reply) const {
    if (reply == nullptr) {
      return;
    }
    if (session.Header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.resultSize > session.Header()->maxResponseBytes) {
      reply->status = StatusCode::ProtocolMismatch;
      return;
    }
    reply->payload.assign(response_bytes, response_bytes + entry.resultSize);
  }

  static void MarkResponseConsumed(ResponseSlotPayload* response_slot) {
    if (response_slot == nullptr) {
      return;
    }
    response_slot->runtime.state = SlotRuntimeStateCode::Consumed;
    response_slot->runtime.lastUpdateMonoMs = MonotonicNowMs();
  }

  void CleanupCompletedSlots(ResponseSlotPayload* response_slot,
                             const std::shared_ptr<RpcFuture::State>& pending,
                             uint32_t response_slot_index,
                             bool responseRing_became_not_full) {
    SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
    if (response_slot != nullptr && pending != nullptr) {
      SlotPayload* request_slot = session.GetSlotPayload(response_slot->response.requestSlotIndex);
      if (request_slot != nullptr) {
        std::memset(&request_slot->runtime, 0, sizeof(request_slot->runtime));
      }
      const bool request_slot_became_available = slot_pool->available() == 0;
      slot_pool->Release(response_slot->response.requestSlotIndex);
      SignalEventFdIfNeeded(session.Handles().reqCreditEventFd,
                            request_slot_became_available &&
                                submitter_waiting_for_credit.load(std::memory_order_relaxed));
    }
    if (response_slot != nullptr) {
      std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    }
    const bool response_slot_became_available = response_slot_pool.Available() == 0;
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
    reply.engineCode = entry.engineErrno;
    reply.detailCode = entry.detailCode;
    ResponseSlotPayload* responseSlot = session.GetResponseSlotPayload(entry.slotIndex);
    uint8_t* responseBytes = session.GetResponseSlotBytes(entry.slotIndex);

    auto lookup = LookupPendingFuture(responseSlot);
    if (HandleMismatchedResponse(entry, responseSlot, lookup, &reply)) {
      return;
    }
    FillReplyPayload(entry, responseSlot, responseBytes, &reply);
    MarkResponseConsumed(responseSlot);
    if (reply.status != StatusCode::Ok && lookup.info.has_value()) {
      NotifyFailure(*lookup.info, reply.status, FailureStage::Response);
    }
    ResolveCompletedFuture(lookup.future, std::move(reply));
    if (lookup.future != nullptr) {
      TouchActivity();
    }
    CleanupCompletedSlots(responseSlot, lookup.future, entry.slotIndex,
                          responseRing_became_not_full);
  }

  void ReleaseEventResponseSlot(SharedSlotPool* response_slot_pool, uint32_t response_slot_index,
                                bool response_ring_became_not_full) const {
    if (response_slot_pool == nullptr) {
      return;
    }
    const bool response_slot_became_available = response_slot_pool->Available() == 0;
    response_slot_pool->Release(response_slot_index);
    SignalEventFdIfNeeded(session.Handles().respCreditEventFd,
                          response_ring_became_not_full || response_slot_became_available);
  }

  RpcEventCallback GetEventCallback() {
    std::lock_guard<std::mutex> lock(event_mutex);
    return event_callback;
  }

  void DeliverEvent(const ResponseRingEntry& entry, bool responseRing_became_not_full) {
    SharedSlotPool response_slot_pool(session.GetResponseSlotPoolRegion());
    RpcEvent event;
    event.eventDomain = entry.eventDomain;
    event.eventType = entry.eventType;
    event.flags = entry.flags;
    ResponseSlotPayload* response_slot = session.GetResponseSlotPayload(entry.slotIndex);
    uint8_t* response_bytes = session.GetResponseSlotBytes(entry.slotIndex);
    if (response_slot != nullptr && response_slot->runtime.requestId != entry.requestId) {
      HILOGW("drop mismatched event request_id, expected=%{public}llu slot=%{public}llu",
            static_cast<unsigned long long>(entry.requestId),
            static_cast<unsigned long long>(response_slot->runtime.requestId));
      session.SetState(Session::SessionState::Broken);
      HandleEngineDeath(current_session_id);
      return;
    }
    if (session.Header() == nullptr || response_slot == nullptr || response_bytes == nullptr ||
        entry.resultSize > session.Header()->maxResponseBytes) {
      HILOGW("drop invalid event, size=%{public}u", entry.resultSize);
      ReleaseEventResponseSlot(&response_slot_pool, entry.slotIndex, responseRing_became_not_full);
      return;
    }
    event.payload.assign(response_bytes, response_bytes + entry.resultSize);

    RpcEventCallback callback = GetEventCallback();
    if (callback) {
      callback(event);
    }
    MarkResponseConsumed(response_slot);
    std::memset(response_slot, 0, sizeof(ResponseSlotPayload));
    ReleaseEventResponseSlot(&response_slot_pool, entry.slotIndex, responseRing_became_not_full);
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

  [[nodiscard]] bool SpinForResponseRing(int iterations) const {
    for (int i = 0; i < iterations; ++i) {
      if (RingCount(session.Header()->responseRing) > 0) {
        return true;
      }
      CpuRelax();
    }
    return false;
  }

  bool PollAndDrainEventFd(pollfd* fd) {
    const auto wait_result = PollEventFd(fd, 100);
    switch (wait_result) {
      case PollEventFdResult::Ready:
      case PollEventFdResult::Timeout:
      case PollEventFdResult::Retry:
        return true;
      case PollEventFdResult::Failed:
        HandleEngineDeath(current_session_id);
        return false;
    }
    return false;
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

  static PendingInfo MakePendingInfoFromCall(const RpcCall& call, uint64_t request_id,
                                             uint64_t session_id = 0) {
    PendingInfo info;
    info.opcode = call.opcode;
    info.priority = call.priority;
    info.flags = call.flags;
    info.admission_timeout_ms = call.admissionTimeoutMs;
    info.queue_timeout_ms = call.queueTimeoutMs;
    info.exec_timeout_ms = call.execTimeoutMs;
    info.request_id = request_id;
    info.session_id = session_id;
    return info;
  }

  RpcFuture MakeFailedInvokeFuture(const RpcCall& call, uint64_t request_id, StatusCode status,
                                   uint64_t session_id = 0) {
    NotifyFailure(MakePendingInfoFromCall(call, request_id, session_id), status,
                  FailureStage::Admission);
    return MakeReadyFuture(status);
  }

  RpcFuture InvokeAsync(RpcCall call) {
    const uint64_t request_id = next_request_id.fetch_add(1);
    const StatusCode ensure_status = EnsureLiveSession();
    if (ensure_status != StatusCode::Ok) {
      return MakeFailedInvokeFuture(call, request_id, ensure_status);
    }

    {
      std::lock_guard<std::mutex> session_lock(session_mutex);
      if (session.Header() != nullptr && call.payload.size() > session.Header()->maxRequestBytes) {
        return MakeFailedInvokeFuture(call, request_id, StatusCode::InvalidArgument);
      }
    }

    StartSubmitter();

    const uint64_t session_id = current_session_id.load(std::memory_order_relaxed);
    std::shared_ptr<RpcFuture::State> pending;
    {
      std::lock_guard<std::mutex> lock(submit_mutex);
      if (submit_queue.size() >= MAX_SUBMIT_QUEUE_SIZE) {
        return MakeFailedInvokeFuture(call, request_id, StatusCode::QueueFull, session_id);
      }
      pending = std::make_shared<RpcFuture::State>();
      PendingSubmit submit;
      submit.call = std::move(call);
      submit.request_id = request_id;
      submit.future = pending;
      submit.session_id = session_id;
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
  const StatusCode status = impl_->EnsureLiveSession();
  if (status == StatusCode::Ok) {
    impl_->StartSubmitter();
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
    stats.queuedSubmissions = static_cast<uint32_t>(impl_->submit_queue.size());
  }
  stats.pendingCalls = impl_->pending_count.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    if (impl_->slot_pool != nullptr) {
      stats.requestSlotCapacity = impl_->slot_pool->capacity();
    }
    if (impl_->session.Header() != nullptr) {
      stats.highRequestRingPending = RingCount(impl_->session.Header()->highRing);
      stats.normalRequestRingPending = RingCount(impl_->session.Header()->normalRing);
      stats.responseRingPending = RingCount(impl_->session.Header()->responseRing);
    }
  }
  stats.waitingForRequestCredit =
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
  if (call.admissionTimeoutMs == 0 && call.queueTimeoutMs == 0 && call.execTimeoutMs == 0) {
    return future.Wait(reply);
  }
  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(call.admissionTimeoutMs) +
      static_cast<int64_t>(call.queueTimeoutMs) + static_cast<int64_t>(call.execTimeoutMs));
  return future.WaitFor(reply, wait_budget);
}

RpcClientRuntimeStats RpcSyncClient::GetRuntimeStats() const {
  return client_.GetRuntimeStats();
}

void RpcSyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace MemRpc
