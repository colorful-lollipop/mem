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

#include "memrpc/core/runtime_utils.h"
#include "core/session.h"

namespace MemRpc {

namespace {

constexpr auto kHealthCheckPeriod = std::chrono::milliseconds(100);
constexpr auto kIdlePollPeriod = std::chrono::milliseconds(100);
constexpr auto kRecoveryRetryPollPeriod = std::chrono::milliseconds(20);

ReplayHint ReplayHintForStatus(StatusCode status) {
  switch (status) {
    case StatusCode::QueueTimeout:
      return ReplayHint::SafeToReplay;
    case StatusCode::ExecTimeout:
    case StatusCode::CrashedDuringExecution:
      return ReplayHint::MaybeExecuted;
    default:
      return ReplayHint::Unknown;
  }
}

FailureStage FailureStageForStatus(StatusCode status) {
  switch (status) {
    case StatusCode::QueueTimeout:
    case StatusCode::ExecTimeout:
      return FailureStage::Timeout;
    default:
      return FailureStage::Response;
  }
}

RecoveryTrigger RecoveryTriggerForStatus(StatusCode status) {
  switch (status) {
    case StatusCode::ExecTimeout:
      return RecoveryTrigger::ExecTimeout;
    default:
      return RecoveryTrigger::Unknown;
  }
}

RecoveryTrigger RecoveryTriggerForSessionOpenReason(SessionOpenReason reason, RecoveryTrigger fallback) {
  switch (reason) {
    case SessionOpenReason::InitialInit:
    case SessionOpenReason::RestartRecovery:
    case SessionOpenReason::ExternalRecovery:
      return fallback;
    case SessionOpenReason::DemandReconnect:
      return RecoveryTrigger::DemandReconnect;
  }
  return fallback;
}

bool IsHighPriority(const RpcCall& call) {
  return call.priority == Priority::High;
}

std::chrono::milliseconds CooldownRemaining(uint64_t cooldownUntilMs) {
  const uint64_t nowMs = MonotonicNowMs64();
  if (cooldownUntilMs <= nowMs) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::milliseconds(cooldownUntilMs - nowMs);
}

}  // namespace

struct RpcFuture::State {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool consumed = false;
  RpcReply reply;
  std::function<void(RpcReply)> callback;
  RpcThenExecutor executor;
};

RpcFuture::RpcFuture() = default;

RpcFuture::RpcFuture(std::shared_ptr<State> state) : state_(std::move(state)) {}

RpcFuture::~RpcFuture() = default;

bool RpcFuture::IsReady() const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ready;
}

StatusCode RpcFuture::Wait(RpcReply* reply) {
  if (!state_) {
    if (reply != nullptr) {
      *reply = RpcReply{};
      reply->status = StatusCode::PeerDisconnected;
    }
    return StatusCode::PeerDisconnected;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->cv.wait(lock, [this] { return state_->ready; });
  if (reply != nullptr) {
    *reply = state_->reply;
  }
  state_->consumed = true;
  return state_->reply.status;
}

StatusCode RpcFuture::WaitAndTake(RpcReply* reply) {
  if (!state_) {
    if (reply != nullptr) {
      *reply = RpcReply{};
      reply->status = StatusCode::PeerDisconnected;
    }
    return StatusCode::PeerDisconnected;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->cv.wait(lock, [this] { return state_->ready; });
  if (reply != nullptr) {
    *reply = std::move(state_->reply);
  }
  state_->consumed = true;
  return reply != nullptr ? reply->status : state_->reply.status;
}

StatusCode RpcFuture::WaitFor(RpcReply* reply, std::chrono::milliseconds timeout) {
  if (!state_) {
    if (reply != nullptr) {
      *reply = RpcReply{};
      reply->status = StatusCode::PeerDisconnected;
    }
    return StatusCode::PeerDisconnected;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->cv.wait_for(lock, timeout, [this] { return state_->ready; })) {
    if (reply != nullptr) {
      *reply = RpcReply{};
      reply->status = StatusCode::QueueTimeout;
    }
    return StatusCode::QueueTimeout;
  }
  if (reply != nullptr) {
    *reply = state_->reply;
  }
  state_->consumed = true;
  return state_->reply.status;
}

void RpcFuture::Then(std::function<void(RpcReply)> callback, RpcThenExecutor executor) {
  if (!state_ || !callback) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->ready) {
    RpcReply reply = state_->reply;
    lock.unlock();
    if (executor) {
      executor([cb = std::move(callback), reply = std::move(reply)]() mutable {
        cb(std::move(reply));
      });
      return;
    }
    callback(std::move(reply));
    return;
  }
  state_->callback = std::move(callback);
  state_->executor = std::move(executor);
}

struct RpcClient::Impl {
  struct SessionSnapshot {
    uint64_t sessionId = 0;
    int reqCreditEventFd = -1;
    int respEventFd = -1;
    int respCreditEventFd = -1;
    bool alive = false;
  };

  struct PendingInfo {
    Opcode opcode = OPCODE_INVALID;
    Priority priority = Priority::Normal;
    uint32_t admissionTimeoutMs = 0;
    uint32_t queueTimeoutMs = 0;
    uint32_t execTimeoutMs = 0;
    uint64_t requestId = 0;
    uint64_t sessionId = 0;
  };

  struct PendingSubmit {
    RpcCall call;
    uint64_t requestId = 0;
    std::shared_ptr<RpcFuture::State> future;
  };

  struct PendingRequest {
    std::shared_ptr<RpcFuture::State> future;
    PendingInfo info;
    std::chrono::steady_clock::time_point waitDeadline =
        std::chrono::steady_clock::time_point::max();
    uint64_t admittedMonoMs = 0;
  };

  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap)
      : bootstrap_(std::move(bootstrap)) {}

  std::shared_ptr<IBootstrapChannel> bootstrap_;
  Session session_;
  mutable std::mutex sessionMutex_;
  mutable std::mutex pendingMutex_;
  mutable std::mutex submitMutex_;
  mutable std::mutex recoveryMutex_;
  mutable std::mutex eventMutex_;
  mutable std::mutex sessionReadyMutex_;
  mutable std::mutex watchdogMutex_;
  RecoveryPolicy recoveryPolicy_;
  RpcEventCallback eventCallback_;
  SessionReadyCallback sessionReadyCallback_;
  RecoveryEventCallback recoveryEventCallback_;
  std::deque<PendingSubmit> submitQueue_;
  std::unordered_map<uint64_t, PendingRequest> pending_;
  std::condition_variable submitCv_;
  std::condition_variable watchdogCv_;
  std::condition_variable recoveryCv_;
  std::thread submitThread_;
  std::thread responseThread_;
  std::thread watchdogThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shuttingDown{false};
  std::atomic<bool> clientClosed_{false};
  std::atomic<bool> submitterWaitingForCredit_{false};
  std::atomic<bool> recoveryPending_{false};
  std::atomic<uint8_t> lifecycleState_{static_cast<uint8_t>(ClientLifecycleState::Uninitialized)};
  std::shared_ptr<const SessionSnapshot> sessionSnapshot_ = std::make_shared<SessionSnapshot>();
  std::atomic<uint64_t> nextRequestId_{1};
  std::atomic<uint64_t> cooldownUntilMs_{0};
  std::atomic<uint64_t> lastActivityMs_{0};
  std::atomic<uint64_t> lastClosedSessionId_{0};
  std::atomic<uint32_t> sessionGeneration_{0};
  SessionOpenReason nextSessionOpenReason_{SessionOpenReason::InitialInit};
  RecoveryTrigger nextSessionOpenTrigger_{RecoveryTrigger::Unknown};
  uint32_t nextSessionOpenDelayMs_ = 0;
  RecoveryTrigger lastRecoveryTrigger_{RecoveryTrigger::Unknown};
  RecoveryAction lastRecoveryAction_{RecoveryAction::Ignore};
  bool terminalManualShutdown_ = false;
  uint64_t lastOpenedSessionId_ = 0;

  static RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(state);
  }

  void TouchActivity() {
    lastActivityMs_.store(MonotonicNowMs64(), std::memory_order_release);
  }

  std::shared_ptr<const SessionSnapshot> LoadSessionSnapshot() const {
    return std::atomic_load_explicit(&sessionSnapshot_, std::memory_order_acquire);
  }

  uint64_t CurrentSessionId() const {
    return LoadSessionSnapshot()->sessionId;
  }

  void PublishSessionSnapshotLocked(const SessionSnapshot& snapshot) {
    std::shared_ptr<const SessionSnapshot> nextSnapshot = std::make_shared<SessionSnapshot>(snapshot);
    std::atomic_store_explicit(&sessionSnapshot_, std::move(nextSnapshot), std::memory_order_release);
  }

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    bootstrap_ = std::move(bootstrap);
    InstallDeathCallbackLocked();
  }

  void SetSessionReadyCallback(SessionReadyCallback callback) {
    std::lock_guard<std::mutex> lock(sessionReadyMutex_);
    sessionReadyCallback_ = std::move(callback);
  }

  void SetRecoveryEventCallback(RecoveryEventCallback callback) {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    recoveryEventCallback_ = std::move(callback);
  }

  ClientLifecycleState LifecycleState() const {
    return static_cast<ClientLifecycleState>(lifecycleState_.load(std::memory_order_acquire));
  }

  RecoveryEventReport MakeRecoveryEventReportLocked(ClientLifecycleState previousState,
                                                    ClientLifecycleState state,
                                                    RecoveryTrigger trigger,
                                                    RecoveryAction action,
                                                    uint32_t cooldownDelayMs) const {
    RecoveryEventReport report;
    report.previousState = previousState;
    report.state = state;
    report.trigger = trigger;
    report.action = action;
    report.terminalManualShutdown = terminalManualShutdown_;
    report.recoveryPending = recoveryPending_.load(std::memory_order_acquire);
    report.cooldownDelayMs = cooldownDelayMs;
    report.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemaining().count());
    report.sessionId = CurrentSessionId();
    report.previousSessionId = lastClosedSessionId_.load(std::memory_order_acquire);
    report.monotonicMs = MonotonicNowMs64();
    return report;
  }

  void EmitRecoveryEvent(const RecoveryEventReport& report, const RecoveryEventCallback& callback) {
    if (callback) {
      callback(report);
    }
  }

  void TransitionLifecycle(ClientLifecycleState state,
                           RecoveryTrigger trigger,
                           RecoveryAction action,
                           uint32_t cooldownDelayMs = 0) {
    RecoveryEventCallback callback;
    RecoveryEventReport report;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      const ClientLifecycleState previousState = LifecycleState();
      lifecycleState_.store(static_cast<uint8_t>(state), std::memory_order_release);
      if (trigger != RecoveryTrigger::Unknown) {
        lastRecoveryTrigger_ = trigger;
      }
      lastRecoveryAction_ = action;
      callback = recoveryEventCallback_;
      report = MakeRecoveryEventReportLocked(previousState, state,
                                             trigger != RecoveryTrigger::Unknown ? trigger
                                                                                 : lastRecoveryTrigger_,
                                             action, cooldownDelayMs);
    }
    EmitRecoveryEvent(report, callback);
  }

  void MarkNextSessionOpen(SessionOpenReason reason, uint32_t delayMs) {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    nextSessionOpenReason_ = reason;
    nextSessionOpenDelayMs_ = delayMs;
  }

  void MarkNextSessionOpen(SessionOpenReason reason, RecoveryTrigger trigger, uint32_t delayMs) {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    nextSessionOpenReason_ = reason;
    nextSessionOpenTrigger_ = trigger;
    nextSessionOpenDelayMs_ = delayMs;
  }

  void PrepareForInitOpen() {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    nextSessionOpenReason_ = sessionGeneration_.load(std::memory_order_acquire) == 0
                                 ? SessionOpenReason::InitialInit
                                 : SessionOpenReason::DemandReconnect;
    nextSessionOpenTrigger_ = RecoveryTrigger::Unknown;
    nextSessionOpenDelayMs_ = 0;
    lastRecoveryTrigger_ = RecoveryTrigger::Unknown;
    lastRecoveryAction_ = RecoveryAction::Ignore;
    terminalManualShutdown_ = false;
    lifecycleState_.store(static_cast<uint8_t>(ClientLifecycleState::Uninitialized),
                          std::memory_order_release);
  }

  void NotifySessionReady(uint64_t sessionId) {
    SessionReadyCallback callback;
    RecoveryTrigger trigger = RecoveryTrigger::Unknown;
    SessionOpenReason reason = SessionOpenReason::InitialInit;
    uint32_t scheduledDelayMs = 0;
    RecoveryAction lastAction = RecoveryAction::Ignore;
    {
      std::lock_guard<std::mutex> lock(sessionReadyMutex_);
      callback = sessionReadyCallback_;
    }

    SessionReadyReport report;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      reason = nextSessionOpenReason_;
      trigger = RecoveryTriggerForSessionOpenReason(nextSessionOpenReason_, nextSessionOpenTrigger_);
      report.reason = reason;
      report.scheduledDelayMs = nextSessionOpenDelayMs_;
      scheduledDelayMs = nextSessionOpenDelayMs_;
      nextSessionOpenReason_ = SessionOpenReason::DemandReconnect;
      nextSessionOpenTrigger_ = RecoveryTrigger::DemandReconnect;
      nextSessionOpenDelayMs_ = 0;
      lastOpenedSessionId_ = sessionId;
      lastAction = lastRecoveryAction_;
    }
    report.sessionId = sessionId;
    report.previousSessionId = lastClosedSessionId_.load(std::memory_order_acquire);
    report.generation = sessionGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    report.monotonicMs = MonotonicNowMs64();
    recoveryPending_.store(false, std::memory_order_release);
    cooldownUntilMs_.store(0, std::memory_order_release);
    TransitionLifecycle(ClientLifecycleState::Active, trigger, lastAction, scheduledDelayMs);

    if (callback) {
      callback(report);
    }
    recoveryCv_.notify_all();
  }

  void InstallDeathCallbackLocked() {
    if (bootstrap_ == nullptr) {
      return;
    }
    bootstrap_->SetEngineDeathCallback([this](uint64_t sessionId) { HandleEngineDeath(sessionId); });
  }

  PendingInfo MakePendingInfo(const PendingSubmit& submit) const {
    PendingInfo info;
    info.opcode = submit.call.opcode;
    info.priority = submit.call.priority;
    info.admissionTimeoutMs = submit.call.admissionTimeoutMs;
    info.queueTimeoutMs = submit.call.queueTimeoutMs;
    info.execTimeoutMs = submit.call.execTimeoutMs;
    info.requestId = submit.requestId;
    info.sessionId = CurrentSessionId();
    return info;
  }

  void ResolveState(const std::shared_ptr<RpcFuture::State>& state, RpcReply reply) {
    if (state == nullptr) {
      return;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->ready) {
      return;
    }
    state->reply = std::move(reply);
    state->ready = true;
    if (state->callback) {
      auto callback = std::move(state->callback);
      auto executor = std::move(state->executor);
      RpcReply callbackReply = state->reply;
      lock.unlock();
      if (executor) {
        executor([callback = std::move(callback), callbackReply = std::move(callbackReply)]() mutable {
          callback(std::move(callbackReply));
        });
      } else {
        callback(std::move(callbackReply));
      }
      return;
    }
    state->cv.notify_all();
  }

  void NotifyFailure(const PendingInfo& info, StatusCode status, FailureStage stage) {
    RecoveryPolicy policy;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      policy = recoveryPolicy_;
    }
    if (!policy.onFailure) {
      return;
    }

    RpcFailure failure;
    failure.status = status;
    failure.opcode = info.opcode;
    failure.priority = info.priority;
    failure.admissionTimeoutMs = info.admissionTimeoutMs;
    failure.queueTimeoutMs = info.queueTimeoutMs;
    failure.execTimeoutMs = info.execTimeoutMs;
    failure.requestId = info.requestId;
    failure.sessionId = info.sessionId;
    failure.monotonicMs = MonotonicNowMs();
    failure.stage = stage;
    failure.replayHint = ReplayHintForStatus(status);
    failure.lastRuntimeState = RpcRuntimeState::Unknown;

    const RecoveryDecision decision = policy.onFailure(failure);
    ApplyRecoveryDecision(decision, RecoveryTriggerForStatus(status), false);
  }

  void ApplyRecoveryDecision(const RecoveryDecision& decision,
                             RecoveryTrigger trigger,
                             bool fromEngineDeath) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    switch (decision.action) {
      case RecoveryAction::Ignore:
        return;
      case RecoveryAction::IdleClose:
        EnterIdleClosed();
        return;
      case RecoveryAction::ManualShutdown:
        return;
      case RecoveryAction::Restart:
        ScheduleRecovery(trigger, SessionOpenReason::RestartRecovery, decision.delayMs, fromEngineDeath);
        return;
    }
  }

  void FailAndResolve(const PendingInfo& info, StatusCode status, FailureStage stage,
                      const std::shared_ptr<RpcFuture::State>& future) {
    NotifyFailure(info, status, stage);
    RpcReply reply;
    reply.status = status;
    ResolveState(future, std::move(reply));
  }

  void FailAllPending(StatusCode status) {
    std::vector<PendingRequest> toFail;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      for (auto& [requestId, pending] : pending_) {
        static_cast<void>(requestId);
        toFail.push_back(std::move(pending));
      }
      pending_.clear();
    }
    for (auto& pending : toFail) {
      if (status == StatusCode::CrashedDuringExecution) {
        RpcReply reply;
        reply.status = status;
        ResolveState(pending.future, std::move(reply));
      } else {
        FailAndResolve(pending.info, status, FailureStageForStatus(status), pending.future);
      }
    }
  }

  std::chrono::steady_clock::time_point MakePendingWaitDeadline(uint32_t execTimeoutMs) const {
    if (execTimeoutMs == 0) {
      return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(execTimeoutMs);
  }

  StatusCode OpenSession() {
    std::shared_ptr<IBootstrapChannel> bootstrap;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      bootstrap = bootstrap_;
      InstallDeathCallbackLocked();
    }
    if (bootstrap == nullptr) {
      return StatusCode::InvalidArgument;
    }

    BootstrapHandles handles;
    const StatusCode openStatus = bootstrap->OpenSession(handles);
    if (openStatus != StatusCode::Ok) {
      return openStatus;
    }

    const uint64_t sessionId = handles.sessionId;
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      const StatusCode attachStatus = session_.Attach(handles, Session::AttachRole::Client);
      if (attachStatus != StatusCode::Ok) {
        return attachStatus;
      }
      session_.SetState(Session::SessionState::Alive);
      SessionSnapshot snapshot;
      snapshot.sessionId = sessionId;
      snapshot.reqCreditEventFd = handles.reqCreditEventFd;
      snapshot.respEventFd = handles.respEventFd;
      snapshot.respCreditEventFd = handles.respCreditEventFd;
      snapshot.alive = true;
      PublishSessionSnapshotLocked(snapshot);
      TouchActivity();
    }
    NotifySessionReady(sessionId);
    return StatusCode::Ok;
  }

  void CloseLiveSession() {
    std::shared_ptr<IBootstrapChannel> bootstrap;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      bootstrap = bootstrap_;
    }
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      const uint64_t closedSessionId = LoadSessionSnapshot()->sessionId;
      PublishSessionSnapshotLocked({});
      session_.Reset();
      if (closedSessionId != 0) {
        lastClosedSessionId_.store(closedSessionId, std::memory_order_release);
      }
    }
    if (bootstrap != nullptr) {
      (void)bootstrap->CloseSession();
    }
  }

  void EnterIdleClosed() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    cooldownUntilMs_.store(0, std::memory_order_release);
    recoveryPending_.store(false, std::memory_order_release);
    CloseLiveSession();
    TransitionLifecycle(ClientLifecycleState::IdleClosed, RecoveryTrigger::IdlePolicy,
                        RecoveryAction::IdleClose);
    recoveryCv_.notify_all();
  }

  void EnterDisconnected(RecoveryTrigger trigger) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    cooldownUntilMs_.store(0, std::memory_order_release);
    recoveryPending_.store(false, std::memory_order_release);
    TransitionLifecycle(ClientLifecycleState::Disconnected, trigger, RecoveryAction::Ignore);
    recoveryCv_.notify_all();
  }

  void EnterTerminalClosed() {
    cooldownUntilMs_.store(0, std::memory_order_release);
    recoveryPending_.store(false, std::memory_order_release);
    clientClosed_.store(true, std::memory_order_release);
    shuttingDown.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      terminalManualShutdown_ = true;
    }
    CloseLiveSession();
    TransitionLifecycle(ClientLifecycleState::Closed, RecoveryTrigger::ManualShutdown,
                        RecoveryAction::ManualShutdown);
    recoveryCv_.notify_all();
  }

  void ScheduleRecovery(RecoveryTrigger trigger,
                        SessionOpenReason openReason,
                        uint32_t delayMs,
                        bool fromEngineDeath) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    MarkNextSessionOpen(openReason, trigger, delayMs);
    recoveryPending_.store(true, std::memory_order_release);
    cooldownUntilMs_.store(MonotonicNowMs64() + delayMs, std::memory_order_release);
    TransitionLifecycle(delayMs == 0 ? ClientLifecycleState::Recovering
                                     : ClientLifecycleState::Cooldown,
                        trigger, RecoveryAction::Restart, delayMs);
    recoveryCv_.notify_all();
    CloseLiveSession();
    if (!fromEngineDeath) {
      FailAllPending(StatusCode::PeerDisconnected);
    }
    if (delayMs == 0) {
      (void)EnsureLiveSession();
    }
  }

  bool CooldownActive() const {
    return MonotonicNowMs64() < cooldownUntilMs_.load(std::memory_order_acquire);
  }

  std::chrono::milliseconds CooldownRemaining() const {
    return ::MemRpc::CooldownRemaining(cooldownUntilMs_.load(std::memory_order_acquire));
  }

  bool RecoveryPending() const {
    return recoveryPending_.load(std::memory_order_acquire);
  }

  StatusCode EnsureLiveSession() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return StatusCode::ClientClosed;
    }
    if (CooldownActive()) {
      return StatusCode::CooldownActive;
    }
    const auto snapshot = LoadSessionSnapshot();
    if (snapshot->alive) {
      return StatusCode::Ok;
    }
    const ClientLifecycleState lifecycleState = LifecycleState();
    RecoveryAction lastAction = RecoveryAction::Ignore;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      lastAction = lastRecoveryAction_;
    }
    if (lifecycleState == ClientLifecycleState::IdleClosed) {
      MarkNextSessionOpen(SessionOpenReason::DemandReconnect, RecoveryTrigger::DemandReconnect, 0);
      recoveryPending_.store(true, std::memory_order_release);
      TransitionLifecycle(ClientLifecycleState::Recovering, RecoveryTrigger::DemandReconnect,
                          RecoveryAction::Ignore);
    } else if (lifecycleState == ClientLifecycleState::Disconnected) {
      MarkNextSessionOpen(SessionOpenReason::DemandReconnect, RecoveryTrigger::DemandReconnect, 0);
      recoveryPending_.store(true, std::memory_order_release);
      TransitionLifecycle(ClientLifecycleState::Recovering, RecoveryTrigger::DemandReconnect,
                          RecoveryAction::Ignore);
    } else if (lifecycleState != ClientLifecycleState::Recovering &&
               lifecycleState != ClientLifecycleState::Uninitialized) {
      recoveryPending_.store(true, std::memory_order_release);
      TransitionLifecycle(ClientLifecycleState::Recovering, RecoveryTrigger::Unknown,
                          lastAction);
    }
    return OpenSession();
  }

  void StartThreads() {
    running_.store(true, std::memory_order_release);
    shuttingDown.store(false, std::memory_order_release);
    submitThread_ = std::thread([this] { SubmitLoop(); });
    responseThread_ = std::thread([this] { ResponseLoop(); });
    watchdogThread_ = std::thread([this] { WatchdogLoop(); });
  }

  void StopThreads() {
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      running_.store(false, std::memory_order_release);
    }
    submitCv_.notify_all();
    {
      std::lock_guard<std::mutex> lock(watchdogMutex_);
      watchdogCv_.notify_all();
    }
    recoveryCv_.notify_all();
    const auto snapshot = LoadSessionSnapshot();
    if (snapshot->reqCreditEventFd >= 0) {
      (void)SignalEventFd(snapshot->reqCreditEventFd);
    }
    if (snapshot->respEventFd >= 0) {
      (void)SignalEventFd(snapshot->respEventFd);
    }
    if (submitThread_.joinable()) {
      submitThread_.join();
    }
    if (responseThread_.joinable()) {
      responseThread_.join();
    }
    if (watchdogThread_.joinable()) {
      watchdogThread_.join();
    }
  }

  void Shutdown() {
    if (clientClosed_.load(std::memory_order_acquire) &&
        LifecycleState() == ClientLifecycleState::Closed) {
      return;
    }
    EnterTerminalClosed();
    StopThreads();
    FailAllPending(StatusCode::ClientClosed);
    std::deque<PendingSubmit> queued;
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      queued.swap(submitQueue_);
    }
    for (auto& submit : queued) {
      RpcReply reply;
      reply.status = StatusCode::ClientClosed;
      ResolveState(submit.future, std::move(reply));
    }
  }

  PollEventFdResult WaitForRequestCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = LoadSessionSnapshot()->reqCreditEventFd;
    if (fd < 0) {
      return PollEventFdResult::Failed;
    }

    submitterWaitingForCredit_.store(true, std::memory_order_release);
    [[maybe_unused]] const auto clearWaiting =
        MakeScopeExit([this] { submitterWaitingForCredit_.store(false, std::memory_order_release); });

    pollfd pollFd{fd, POLLIN, 0};
    while (running_.load(std::memory_order_acquire)) {
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

  StatusCode WaitForRecovery(std::chrono::steady_clock::time_point deadline) {
    std::mutex waitMutex;
    std::unique_lock<std::mutex> lock(waitMutex);
    while (running_.load(std::memory_order_acquire)) {
      if (clientClosed_.load(std::memory_order_acquire)) {
        return StatusCode::ClientClosed;
      }

      const uint64_t cooldownUntil = cooldownUntilMs_.load(std::memory_order_acquire);
      const uint64_t nowMs = MonotonicNowMs64();
      if (nowMs >= cooldownUntil) {
        return StatusCode::Ok;
      }

      const auto now = std::chrono::steady_clock::now();
      if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
        return StatusCode::CooldownActive;
      }

      auto wakeAt = now + std::chrono::milliseconds(cooldownUntil - nowMs);
      if (deadline != std::chrono::steady_clock::time_point::max()) {
        wakeAt = std::min(wakeAt, deadline);
      }
      recoveryCv_.wait_until(lock, wakeAt);
    }
    return StatusCode::PeerDisconnected;
  }

  StatusCode WaitForRecoveryRetry(std::chrono::steady_clock::time_point deadline) {
    std::mutex waitMutex;
    std::unique_lock<std::mutex> lock(waitMutex);
    while (running_.load(std::memory_order_acquire)) {
      if (clientClosed_.load(std::memory_order_acquire)) {
        return StatusCode::ClientClosed;
      }
      if (!RecoveryPending()) {
        return StatusCode::PeerDisconnected;
      }

      const auto now = std::chrono::steady_clock::now();
      if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
        return StatusCode::PeerDisconnected;
      }

      auto waitFor = kRecoveryRetryPollPeriod;
      if (deadline != std::chrono::steady_clock::time_point::max()) {
        waitFor = std::min(waitFor,
                           std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
      }
      if (waitFor <= std::chrono::milliseconds::zero()) {
        return StatusCode::PeerDisconnected;
      }
      recoveryCv_.wait_for(lock, waitFor);
      return StatusCode::Ok;
    }
    return StatusCode::PeerDisconnected;
  }

  StatusCode TryPushRequest(const PendingSubmit& submit) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (!session_.Valid() || session_.Header() == nullptr) {
      return StatusCode::PeerDisconnected;
    }
    if (submit.call.payload.size() > session_.Header()->maxRequestBytes ||
        submit.call.payload.size() > RequestRingEntry::INLINE_PAYLOAD_BYTES) {
      return StatusCode::PayloadTooLarge;
    }

    RequestRingEntry entry;
    entry.requestId = submit.requestId;
    entry.enqueueMonoMs = MonotonicNowMs();
    entry.queueTimeoutMs = submit.call.queueTimeoutMs;
    entry.execTimeoutMs = submit.call.execTimeoutMs;
    entry.opcode = submit.call.opcode;
    entry.priority = static_cast<uint8_t>(submit.call.priority);
    entry.payloadSize = static_cast<uint32_t>(submit.call.payload.size());
    if (!submit.call.payload.empty()) {
      std::memcpy(entry.payload.data(), submit.call.payload.data(), submit.call.payload.size());
    }

    PendingRequest pending;
    pending.future = submit.future;
    pending.info = MakePendingInfo(submit);
    pending.info.sessionId = CurrentSessionId();
    pending.waitDeadline = MakePendingWaitDeadline(submit.call.execTimeoutMs);
    pending.admittedMonoMs = MonotonicNowMs64();
    {
      std::lock_guard<std::mutex> pendingLock(pendingMutex_);
      pending_[submit.requestId] = pending;
    }

    const bool highPriority = IsHighPriority(submit.call);
    const StatusCode status =
        session_.PushRequest(highPriority ? QueueKind::HighRequest : QueueKind::NormalRequest, entry);
    if (status != StatusCode::Ok) {
      std::lock_guard<std::mutex> pendingLock(pendingMutex_);
      pending_.erase(submit.requestId);
      return status;
    }

    const RingCursor& cursor =
        highPriority ? session_.Header()->highRing : session_.Header()->normalRing;
    if (RingCountIsOneAfterPush(cursor)) {
      const int fd = highPriority ? session_.Handles().highReqEventFd : session_.Handles().normalReqEventFd;
      (void)SignalEventFd(fd);
    }
    TouchActivity();
    return StatusCode::Ok;
  }

  void SubmitOne(const PendingSubmit& submit) {
    const bool infiniteWait = submit.call.admissionTimeoutMs == 0;
    const auto deadline = infiniteWait ? std::chrono::steady_clock::time_point::max()
                                       : std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(submit.call.admissionTimeoutMs);
    const bool waitForRecovery = submit.call.waitForRecovery;
    const bool infiniteRecoveryWait = submit.call.recoveryTimeoutMs == 0;
    const auto recoveryDeadline =
        !waitForRecovery ? std::chrono::steady_clock::time_point::max()
        : infiniteRecoveryWait ? std::chrono::steady_clock::time_point::max()
                               : std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(submit.call.recoveryTimeoutMs);
    PendingInfo info = MakePendingInfo(submit);

    while (running_.load(std::memory_order_acquire)) {
      const StatusCode sessionStatus = EnsureLiveSession();
      if (sessionStatus == StatusCode::CooldownActive && waitForRecovery) {
        const StatusCode waitStatus = WaitForRecovery(recoveryDeadline);
        if (waitStatus == StatusCode::Ok) {
          continue;
        }
        FailAndResolve(info, waitStatus, FailureStage::Admission, submit.future);
        return;
      }
      if (sessionStatus == StatusCode::PeerDisconnected && waitForRecovery) {
        const StatusCode waitStatus = WaitForRecoveryRetry(recoveryDeadline);
        if (waitStatus == StatusCode::Ok) {
          continue;
        }
        FailAndResolve(info, waitStatus, FailureStage::Admission, submit.future);
        return;
      }
      if (sessionStatus != StatusCode::Ok) {
        FailAndResolve(info, sessionStatus, FailureStage::Admission, submit.future);
        return;
      }

      const StatusCode pushStatus = TryPushRequest(submit);
      if (pushStatus == StatusCode::Ok) {
        return;
      }
      if (pushStatus == StatusCode::PayloadTooLarge) {
        FailAndResolve(info, pushStatus, FailureStage::Admission, submit.future);
        return;
      }
      if (pushStatus == StatusCode::PeerDisconnected) {
        CloseLiveSession();
        if (!infiniteWait && DeadlineReached(deadline)) {
          FailAndResolve(info, StatusCode::QueueTimeout, FailureStage::Admission, submit.future);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (pushStatus != StatusCode::QueueFull) {
        FailAndResolve(info, pushStatus, FailureStage::Admission, submit.future);
        return;
      }

      if (!infiniteWait && DeadlineReached(deadline)) {
        FailAndResolve(info, StatusCode::QueueTimeout, FailureStage::Admission, submit.future);
        return;
      }
      const auto waitResult = WaitForRequestCredit(deadline);
      if (waitResult == PollEventFdResult::Ready || waitResult == PollEventFdResult::Retry) {
        continue;
      }
      if (waitResult == PollEventFdResult::Timeout) {
        FailAndResolve(info, infiniteWait ? StatusCode::QueueFull : StatusCode::QueueTimeout,
                       FailureStage::Admission, submit.future);
        return;
      }
      CloseLiveSession();
    }

    FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission, submit.future);
  }

  void SubmitLoop() {
    while (running_.load(std::memory_order_acquire)) {
      PendingSubmit submit;
      {
        std::unique_lock<std::mutex> lock(submitMutex_);
        submitCv_.wait(lock, [this] {
          return !running_.load(std::memory_order_acquire) || !submitQueue_.empty();
        });
        if (!running_.load(std::memory_order_acquire) && submitQueue_.empty()) {
          break;
        }
        submit = std::move(submitQueue_.front());
        submitQueue_.pop_front();
      }
      SubmitOne(submit);
    }
  }

  void MaybeRunPendingTimeouts() {
    std::vector<PendingRequest> expired;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        const auto deadline = it->second.waitDeadline;
        if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
          expired.push_back(std::move(it->second));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& pending : expired) {
      FailAndResolve(pending.info, StatusCode::ExecTimeout, FailureStage::Timeout, pending.future);
    }
  }

  void ResolveCompletedRequest(const ResponseRingEntry& entry) {
    PendingRequest pending;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      const auto it = pending_.find(entry.requestId);
      if (it != pending_.end()) {
        // pending_ ownership decides terminal completion. If the watchdog has
        // already erased this request, the real reply is late and must be ignored.
        pending = std::move(it->second);
        pending_.erase(it);
        found = true;
      }
    }
    if (!found) {
      return;
    }

    RpcReply reply;
    reply.status = static_cast<StatusCode>(entry.statusCode);
    reply.errorCode = entry.errorCode;
    reply.payload.assign(entry.payload.begin(), entry.payload.begin() + entry.resultSize);
    if (reply.status != StatusCode::Ok) {
      NotifyFailure(pending.info, reply.status, FailureStageForStatus(reply.status));
    }
    ResolveState(pending.future, std::move(reply));
    TouchActivity();
  }

  void DeliverEvent(const ResponseRingEntry& entry) {
    RpcEventCallback callback;
    {
      std::lock_guard<std::mutex> lock(eventMutex_);
      callback = eventCallback_;
    }
    if (!callback) {
      return;
    }

    RpcEvent event;
    event.eventDomain = entry.eventDomain;
    event.eventType = entry.eventType;
    event.flags = entry.flags;
    event.payload.assign(entry.payload.begin(), entry.payload.begin() + entry.resultSize);
    callback(event);
    TouchActivity();
  }

  bool DrainResponseRing() {
    bool drained = false;
    while (true) {
      ResponseRingEntry entry;
      bool ringBecameNotFull = false;
      {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!session_.Valid() || session_.Header() == nullptr) {
          return drained;
        }
        const RingCursor& cursor = session_.Header()->responseRing;
        ringBecameNotFull = cursor.capacity != 0 && RingCount(cursor) == cursor.capacity;
        if (!session_.PopResponse(&entry)) {
          return drained;
        }
      }

      drained = true;
      if (ringBecameNotFull) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        (void)SignalEventFd(session_.Handles().respCreditEventFd);
      }

      if (entry.resultSize > ResponseRingEntry::INLINE_PAYLOAD_BYTES) {
        HandleEngineDeath(CurrentSessionId());
        return true;
      }
      if (entry.messageKind == ResponseMessageKind::Event) {
        DeliverEvent(entry);
      } else {
        ResolveCompletedRequest(entry);
      }
    }
  }

  void ResponseLoop() {
    int activePollFd = -1;
    uint64_t activePollSessionId = 0;
    const auto resetActivePollFd = [&]() {
      if (activePollFd >= 0) {
        (void)close(activePollFd);
        activePollFd = -1;
      }
      activePollSessionId = 0;
    };
    const auto closeActivePollFd = MakeScopeExit([&]() { resetActivePollFd(); });

    while (running_.load(std::memory_order_acquire)) {
      if (DrainResponseRing()) {
        continue;
      }

      const auto snapshot = LoadSessionSnapshot();
      const uint64_t currentSessionId = snapshot->sessionId;
      int nextPollFd = -1;
      bool sessionChanged = false;
      if (currentSessionId != activePollSessionId) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_.Valid()) {
          const uint64_t lockedSessionId = LoadSessionSnapshot()->sessionId;
          if (lockedSessionId != activePollSessionId) {
            sessionChanged = true;
            const int responseFd = session_.Handles().respEventFd;
            if (responseFd >= 0) {
              nextPollFd = dup(responseFd);
            }
          }
        }
      } else if (currentSessionId == 0 && activePollSessionId != 0) {
        sessionChanged = true;
      }

      if (sessionChanged) {
        resetActivePollFd();
        if (nextPollFd >= 0) {
          activePollFd = nextPollFd;
          activePollSessionId = snapshot->sessionId;
        }
      }

      if (activePollFd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      pollfd pollFd{activePollFd, POLLIN, 0};
      const auto waitResult = PollEventFd(&pollFd, 100);
      if (waitResult == PollEventFdResult::Failed) {
        const uint64_t failedSessionId = activePollSessionId;
        resetActivePollFd();
        HandleEngineDeath(failedSessionId);
      }
    }
  }

  void MaybeRunIdlePolicy() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    RecoveryPolicy policy;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      policy = recoveryPolicy_;
    }
    if (!policy.onIdle) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      if (!pending_.empty()) {
        return;
      }
    }
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      if (!submitQueue_.empty()) {
        return;
      }
    }
    const uint64_t sessionId = CurrentSessionId();
    if (sessionId == 0) {
      return;
    }
    const uint64_t nowMs = MonotonicNowMs64();
    const uint64_t idleMs = nowMs - lastActivityMs_.load(std::memory_order_acquire);
    const RecoveryDecision decision = policy.onIdle(idleMs);
    ApplyRecoveryDecision(decision, RecoveryTrigger::IdlePolicy, false);
  }

  void MaybeRunHealthCheck() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    std::shared_ptr<IBootstrapChannel> bootstrap;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      bootstrap = bootstrap_;
    }
    if (bootstrap == nullptr) {
      return;
    }
    const uint64_t expectedSessionId = CurrentSessionId();
    if (expectedSessionId == 0) {
      return;
    }

    const ChannelHealthResult result = bootstrap->CheckHealth(expectedSessionId);
    switch (result.status) {
      case ChannelHealthStatus::Healthy:
      case ChannelHealthStatus::Unsupported:
        return;
      case ChannelHealthStatus::Timeout:
        RequestExternalRecovery({ExternalRecoverySignal::ChannelHealthTimeout, expectedSessionId, 0});
        return;
      case ChannelHealthStatus::Malformed:
        RequestExternalRecovery({ExternalRecoverySignal::ChannelHealthMalformed, expectedSessionId, 0});
        return;
      case ChannelHealthStatus::Unhealthy:
        RequestExternalRecovery({ExternalRecoverySignal::ChannelHealthUnhealthy, expectedSessionId, 0});
        return;
      case ChannelHealthStatus::SessionMismatch:
        RequestExternalRecovery(
            {ExternalRecoverySignal::ChannelHealthSessionMismatch, expectedSessionId, 0});
        return;
    }
  }

  void WatchdogLoop() {
    while (running_.load(std::memory_order_acquire)) {
      MaybeRunPendingTimeouts();
      MaybeRunHealthCheck();
      MaybeRunIdlePolicy();
      std::unique_lock<std::mutex> lock(watchdogMutex_);
      watchdogCv_.wait_for(lock, std::min(kHealthCheckPeriod, kIdlePollPeriod), [this] {
        return !running_.load(std::memory_order_acquire);
      });
    }
  }

  void HandleEngineDeath(uint64_t sessionId) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    const uint64_t current = CurrentSessionId();
    if (sessionId != 0 && current != 0 && sessionId != current) {
      return;
    }

    CloseLiveSession();
    recoveryPending_.store(false, std::memory_order_release);
    cooldownUntilMs_.store(0, std::memory_order_release);
    FailAllPending(StatusCode::CrashedDuringExecution);

    RecoveryPolicy policy;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      policy = recoveryPolicy_;
    }
    if (!policy.onEngineDeath) {
      EnterDisconnected(RecoveryTrigger::EngineDeath);
      return;
    }
    EngineDeathReport report;
    report.deadSessionId = sessionId == 0 ? current : sessionId;
    const RecoveryDecision decision = policy.onEngineDeath(report);
    if (decision.action == RecoveryAction::Ignore) {
      EnterDisconnected(RecoveryTrigger::EngineDeath);
      return;
    }
    ApplyRecoveryDecision(decision, RecoveryTrigger::EngineDeath, true);
  }

  void RequestExternalRecovery(ExternalRecoveryRequest request) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    const uint64_t current = CurrentSessionId();
    if (request.sessionId != 0 && current != 0 && request.sessionId != current) {
      return;
    }
    ScheduleRecovery(RecoveryTrigger::ExternalHealthSignal,
                     SessionOpenReason::ExternalRecovery,
                     request.delayMs, false);
  }

  RpcFuture InvokeAsync(RpcCall call) {
    if (!running_.load(std::memory_order_acquire)) {
      const StatusCode status = clientClosed_.load(std::memory_order_acquire)
                                    ? StatusCode::ClientClosed
                                    : StatusCode::PeerDisconnected;
      auto state = std::make_shared<RpcFuture::State>();
      const uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
      PendingInfo info;
      info.opcode = call.opcode;
      info.priority = call.priority;
      info.admissionTimeoutMs = call.admissionTimeoutMs;
      info.queueTimeoutMs = call.queueTimeoutMs;
      info.execTimeoutMs = call.execTimeoutMs;
      info.requestId = requestId;
      info.sessionId = CurrentSessionId();
      if (status != StatusCode::ClientClosed) {
        NotifyFailure(info, status, FailureStage::Admission);
      }
      RpcReply reply;
      reply.status = status;
      ResolveState(state, std::move(reply));
      return RpcFuture(state);
    }
    auto futureState = std::make_shared<RpcFuture::State>();
    PendingSubmit submit;
    submit.requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    submit.call = std::move(call);
    submit.future = futureState;

    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      submitQueue_.push_back(std::move(submit));
    }
    submitCv_.notify_one();
    return RpcFuture(futureState);
  }

  RpcClientRuntimeStats GetRuntimeStats() const {
    RpcClientRuntimeStats stats;
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      stats.queuedSubmissions = static_cast<uint32_t>(submitQueue_.size());
    }
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      stats.pendingCalls = static_cast<uint32_t>(pending_.size());
    }
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      if (session_.Header() != nullptr) {
        stats.highRequestRingPending = RingCount(session_.Header()->highRing);
        stats.normalRequestRingPending = RingCount(session_.Header()->normalRing);
        stats.responseRingPending = RingCount(session_.Header()->responseRing);
      }
    }
    stats.waitingForRequestCredit = submitterWaitingForCredit_.load(std::memory_order_acquire);
    stats.recoveryPending = RecoveryPending();
    stats.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemaining().count());
    return stats;
  }

  RecoveryRuntimeSnapshot GetRecoveryRuntimeSnapshot() const {
    RecoveryRuntimeSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      snapshot.lifecycleState = LifecycleState();
      snapshot.lastTrigger = lastRecoveryTrigger_;
      snapshot.lastRecoveryAction = lastRecoveryAction_;
      snapshot.terminalManualShutdown = terminalManualShutdown_;
      snapshot.lastOpenedSessionId = lastOpenedSessionId_;
    }
    snapshot.recoveryPending = RecoveryPending();
    snapshot.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemaining().count());
    snapshot.currentSessionId = CurrentSessionId();
    snapshot.lastClosedSessionId = lastClosedSessionId_.load(std::memory_order_acquire);
    return snapshot;
  }
};

RpcClient::RpcClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : impl_(std::make_unique<Impl>(std::move(bootstrap))) {}

RpcClient::~RpcClient() {
  Shutdown();
}

void RpcClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  impl_->SetBootstrapChannel(std::move(bootstrap));
}

void RpcClient::SetEventCallback(RpcEventCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->eventMutex_);
  impl_->eventCallback_ = std::move(callback);
}

void RpcClient::SetSessionReadyCallback(SessionReadyCallback callback) {
  impl_->SetSessionReadyCallback(std::move(callback));
}

void RpcClient::SetRecoveryEventCallback(RecoveryEventCallback callback) {
  impl_->SetRecoveryEventCallback(std::move(callback));
}

void RpcClient::SetRecoveryPolicy(RecoveryPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->recoveryMutex_);
  impl_->recoveryPolicy_ = std::move(policy);
}

void RpcClient::RequestExternalRecovery(ExternalRecoveryRequest request) {
  impl_->RequestExternalRecovery(request);
}

StatusCode RpcClient::Init() {
  if (impl_->clientClosed_.load(std::memory_order_acquire)) {
    return StatusCode::ClientClosed;
  }
  if (impl_->running_.load(std::memory_order_acquire)) {
    return impl_->EnsureLiveSession();
  }
  impl_->recoveryPending_.store(false, std::memory_order_release);
  impl_->cooldownUntilMs_.store(0, std::memory_order_release);
  impl_->lastActivityMs_.store(MonotonicNowMs64(), std::memory_order_release);
  impl_->PrepareForInitOpen();
  impl_->StartThreads();
  const StatusCode status = impl_->EnsureLiveSession();
  if (status != StatusCode::Ok) {
    impl_->Shutdown();
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
  return impl_->GetRuntimeStats();
}

RecoveryRuntimeSnapshot RpcClient::GetRecoveryRuntimeSnapshot() const {
  return impl_->GetRecoveryRuntimeSnapshot();
}

void RpcClient::Shutdown() {
  impl_->Shutdown();
}

RpcSyncClient::RpcSyncClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap)) {}

RpcSyncClient::~RpcSyncClient() = default;

void RpcSyncClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  client_.SetBootstrapChannel(std::move(bootstrap));
}

void RpcSyncClient::SetEventCallback(RpcEventCallback callback) {
  client_.SetEventCallback(std::move(callback));
}

void RpcSyncClient::SetSessionReadyCallback(SessionReadyCallback callback) {
  client_.SetSessionReadyCallback(std::move(callback));
}

void RpcSyncClient::SetRecoveryEventCallback(RecoveryEventCallback callback) {
  client_.SetRecoveryEventCallback(std::move(callback));
}

void RpcSyncClient::SetRecoveryPolicy(RecoveryPolicy policy) {
  client_.SetRecoveryPolicy(std::move(policy));
}

StatusCode RpcSyncClient::Init() {
  return client_.Init();
}

StatusCode RpcSyncClient::InvokeSync(const RpcCall& call, RpcReply* reply) {
  return client_.InvokeAsync(call).Wait(reply);
}

RpcClientRuntimeStats RpcSyncClient::GetRuntimeStats() const {
  return client_.GetRuntimeStats();
}

RecoveryRuntimeSnapshot RpcSyncClient::GetRecoveryRuntimeSnapshot() const {
  return client_.GetRecoveryRuntimeSnapshot();
}

void RpcSyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace MemRpc
