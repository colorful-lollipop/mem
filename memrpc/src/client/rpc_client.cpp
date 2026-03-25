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
#include "virus_protection_service_log.h"

namespace MemRpc {

namespace {

// File-local policy and timing helpers shared by the internal client components.
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

bool ShouldRetryRecoveryStatus(StatusCode status, const RecoveryRuntimeSnapshot& snapshot) {
  if (snapshot.terminalManualShutdown ||
      snapshot.lifecycleState == ClientLifecycleState::Closed) {
    return false;
  }
  if (status == StatusCode::CooldownActive) {
    return snapshot.lifecycleState == ClientLifecycleState::Cooldown ||
           snapshot.lifecycleState == ClientLifecycleState::Recovering;
  }
  if (status == StatusCode::PeerDisconnected) {
    return snapshot.recoveryPending ||
           snapshot.lifecycleState == ClientLifecycleState::Recovering ||
           snapshot.lifecycleState == ClientLifecycleState::Cooldown;
  }
  return false;
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
    HILOGE("RpcFuture::Wait failed: state is null");
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
    HILOGE("RpcFuture::WaitAndTake failed: state is null");
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
    HILOGE("RpcFuture::WaitFor failed: state is null timeout_ms=%{public}lld",
           static_cast<long long>(timeout.count()));
    if (reply != nullptr) {
      *reply = RpcReply{};
      reply->status = StatusCode::PeerDisconnected;
    }
    return StatusCode::PeerDisconnected;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->cv.wait_for(lock, timeout, [this] { return state_->ready; })) {
    HILOGW("RpcFuture::WaitFor timed out timeout_ms=%{public}lld",
           static_cast<long long>(timeout.count()));
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
  if (!state_) {
    HILOGE("RpcFuture::Then ignored: state is null");
    return;
  }
  if (!callback) {
    HILOGE("RpcFuture::Then ignored: callback is null");
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

  struct PendingSubmit {
    RpcCall call;
    uint64_t requestId = 0;
    std::shared_ptr<RpcFuture::State> future;
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

  struct PendingRequest {
    std::shared_ptr<RpcFuture::State> future;
    PendingInfo info;
    std::chrono::steady_clock::time_point waitDeadline =
        std::chrono::steady_clock::time_point::max();
    uint64_t admittedMonoMs = 0;
  };

  class PendingRequestStore {
   public:
    void Put(uint64_t requestId, PendingRequest pending) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_[requestId] = std::move(pending);
    }

    void Erase(uint64_t requestId) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(requestId);
    }

    std::optional<PendingRequest> Take(uint64_t requestId) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = pending_.find(requestId);
      if (it == pending_.end()) {
        return std::nullopt;
      }
      PendingRequest pending = std::move(it->second);
      pending_.erase(it);
      return pending;
    }

    std::vector<PendingRequest> TakeAll() {
      std::vector<PendingRequest> drained;
      std::lock_guard<std::mutex> lock(mutex_);
      drained.reserve(pending_.size());
      for (auto& [requestId, pending] : pending_) {
        static_cast<void>(requestId);
        drained.push_back(std::move(pending));
      }
      pending_.clear();
      return drained;
    }

    std::vector<PendingRequest> TakeExpired(std::chrono::steady_clock::time_point now) {
      std::vector<PendingRequest> expired;
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        const auto deadline = it->second.waitDeadline;
        if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
          expired.push_back(std::move(it->second));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
      return expired;
    }

    [[nodiscard]] bool Empty() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return pending_.empty();
    }

    [[nodiscard]] uint32_t Size() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return static_cast<uint32_t>(pending_.size());
    }

   private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PendingRequest> pending_;
  };

  struct SubmitConfig {
    bool infiniteWait = false;
    bool waitForRecovery = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    std::chrono::steady_clock::time_point recoveryDeadline =
        std::chrono::steady_clock::time_point::max();
  };

  enum class SubmitAction {
    Proceed,
    Retry,
    Finish,
  };

  class SessionController {
   public:
    explicit SessionController(std::shared_ptr<IBootstrapChannel> bootstrap)
        : bootstrap_(std::move(bootstrap)) {}

    void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap,
                             const std::function<void(uint64_t)>& deathCallback) {
      std::lock_guard<std::mutex> lock(bootstrapMutex_);
      if (bootstrap_ != nullptr && bootstrap_ != bootstrap) {
        bootstrap_->SetEngineDeathCallback({});
      }
      bootstrap_ = std::move(bootstrap);
      InstallDeathCallbackLocked(deathCallback);
    }

    void InstallDeathCallback(const std::function<void(uint64_t)>& deathCallback) {
      std::lock_guard<std::mutex> lock(bootstrapMutex_);
      InstallDeathCallbackLocked(deathCallback);
    }

    void ClearDeathCallback() {
      std::lock_guard<std::mutex> lock(bootstrapMutex_);
      if (bootstrap_ != nullptr) {
        bootstrap_->SetEngineDeathCallback({});
      }
    }

    std::shared_ptr<IBootstrapChannel> LoadBootstrap() const {
      std::lock_guard<std::mutex> lock(bootstrapMutex_);
      return bootstrap_;
    }

    std::shared_ptr<const SessionSnapshot> LoadSnapshot() const {
      return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    uint64_t CurrentSessionId() const {
      return LoadSnapshot()->sessionId;
    }

    bool HasLiveSession() const {
      return LoadSnapshot()->alive;
    }

    StatusCode OpenSession() {
      auto bootstrap = LoadBootstrap();
      if (bootstrap == nullptr) {
        HILOGE("RpcClient::OpenSession failed: bootstrap channel is null");
        return StatusCode::InvalidArgument;
      }

      BootstrapHandles handles = MakeDefaultBootstrapHandles();
      const StatusCode openStatus = bootstrap->OpenSession(handles);
      if (openStatus != StatusCode::Ok) {
        HILOGE("RpcClient::OpenSession failed: bootstrap OpenSession status=%{public}d",
               static_cast<int>(openStatus));
        return openStatus;
      }

      const uint64_t sessionId = handles.sessionId;
      std::lock_guard<std::mutex> lock(sessionMutex_);
      const StatusCode attachStatus = session_.Attach(handles, Session::AttachRole::Client);
      if (attachStatus != StatusCode::Ok) {
        HILOGE("RpcClient::OpenSession failed: session attach status=%{public}d session_id=%{public}llu",
               static_cast<int>(attachStatus),
               static_cast<unsigned long long>(sessionId));
        return attachStatus;
      }
      session_.SetState(Session::SessionState::Alive);
      SessionSnapshot snapshot;
      snapshot.sessionId = sessionId;
      snapshot.reqCreditEventFd = handles.reqCreditEventFd;
      snapshot.respEventFd = handles.respEventFd;
      snapshot.respCreditEventFd = handles.respCreditEventFd;
      snapshot.alive = true;
      PublishSnapshotLocked(snapshot);
      return StatusCode::Ok;
    }

    uint64_t CloseLiveSession() {
      const auto bootstrap = LoadBootstrap();
      uint64_t closedSessionId = 0;
      {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        closedSessionId = LoadSnapshot()->sessionId;
        PublishSnapshotLocked({});
        session_.Reset();
      }
      if (bootstrap != nullptr) {
        const StatusCode status = bootstrap->CloseSession();
        if (status != StatusCode::Ok) {
          HILOGW("RpcClient::CloseLiveSession bootstrap CloseSession failed: status=%{public}d",
                 static_cast<int>(status));
        }
      }
      return closedSessionId;
    }

    template <typename Fn>
    auto WithSessionLocked(Fn&& fn) -> decltype(fn(std::declval<Session&>())) {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      return fn(session_);
    }

    template <typename Fn>
    auto WithSessionLocked(Fn&& fn) const -> decltype(fn(std::declval<const Session&>())) {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      return fn(session_);
    }

   private:
    void InstallDeathCallbackLocked(const std::function<void(uint64_t)>& deathCallback) {
      if (bootstrap_ == nullptr) {
        return;
      }
      bootstrap_->SetEngineDeathCallback(deathCallback);
    }

    void PublishSnapshotLocked(const SessionSnapshot& snapshot) {
      std::shared_ptr<const SessionSnapshot> nextSnapshot = std::make_shared<SessionSnapshot>(snapshot);
      std::atomic_store_explicit(&snapshot_, std::move(nextSnapshot), std::memory_order_release);
    }

    mutable std::mutex bootstrapMutex_;
    std::shared_ptr<IBootstrapChannel> bootstrap_;
    Session session_;
    mutable std::mutex sessionMutex_;
    std::shared_ptr<const SessionSnapshot> snapshot_ = std::make_shared<SessionSnapshot>();
  };

  class RecoveryCoordinator {
   public:
    struct SessionReadyState {
      RecoveryTrigger trigger = RecoveryTrigger::Unknown;
      SessionOpenReason reason = SessionOpenReason::InitialInit;
      uint32_t scheduledDelayMs = 0;
      RecoveryAction lastAction = RecoveryAction::Ignore;
    };

    void SetRecoveryEventCallback(RecoveryEventCallback callback) {
      std::lock_guard<std::mutex> lock(mutex_);
      recoveryEventCallback_ = std::move(callback);
    }

    void ClearRecoveryEventCallback() {
      std::lock_guard<std::mutex> lock(mutex_);
      recoveryEventCallback_ = {};
    }

    ClientLifecycleState LifecycleState() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return lifecycleState_;
    }

    void PrepareForInitOpen(bool firstGeneration) {
      std::lock_guard<std::mutex> lock(mutex_);
      nextSessionOpenReason_ = firstGeneration ? SessionOpenReason::InitialInit
                                               : SessionOpenReason::DemandReconnect;
      nextSessionOpenTrigger_ = RecoveryTrigger::Unknown;
      nextSessionOpenDelayMs_ = 0;
      lastRecoveryTrigger_ = RecoveryTrigger::Unknown;
      lastRecoveryAction_ = RecoveryAction::Ignore;
      terminalManualShutdown_ = false;
      recoveryPending_ = false;
      cooldownUntilMs_ = 0;
      lifecycleState_ = ClientLifecycleState::Uninitialized;
    }

    void MarkNextSessionOpen(SessionOpenReason reason, uint32_t delayMs) {
      std::lock_guard<std::mutex> lock(mutex_);
      nextSessionOpenReason_ = reason;
      nextSessionOpenDelayMs_ = delayMs;
    }

    void MarkNextSessionOpen(SessionOpenReason reason, RecoveryTrigger trigger, uint32_t delayMs) {
      std::lock_guard<std::mutex> lock(mutex_);
      nextSessionOpenReason_ = reason;
      nextSessionOpenTrigger_ = trigger;
      nextSessionOpenDelayMs_ = delayMs;
    }

    SessionReadyState ConsumeSessionReady(uint64_t sessionId) {
      std::lock_guard<std::mutex> lock(mutex_);
      SessionReadyState state;
      state.reason = nextSessionOpenReason_;
      state.trigger = RecoveryTriggerForSessionOpenReason(nextSessionOpenReason_, nextSessionOpenTrigger_);
      state.scheduledDelayMs = nextSessionOpenDelayMs_;
      state.lastAction = lastRecoveryAction_;
      nextSessionOpenReason_ = SessionOpenReason::DemandReconnect;
      nextSessionOpenTrigger_ = RecoveryTrigger::DemandReconnect;
      nextSessionOpenDelayMs_ = 0;
      lastOpenedSessionId_ = sessionId;
      recoveryPending_ = false;
      cooldownUntilMs_ = 0;
      return state;
    }

    void ClearRecoveryWindow() {
      std::lock_guard<std::mutex> lock(mutex_);
      recoveryPending_ = false;
      cooldownUntilMs_ = 0;
    }

    void SetTerminalManualShutdown() {
      std::lock_guard<std::mutex> lock(mutex_);
      terminalManualShutdown_ = true;
    }

    void TransitionLifecycle(ClientLifecycleState state,
                             RecoveryTrigger trigger,
                             RecoveryAction action,
                             uint32_t cooldownDelayMs,
                             uint64_t currentSessionId,
                             uint64_t lastClosedSessionId) {
      RecoveryEventCallback callback;
      RecoveryEventReport report;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const ClientLifecycleState previousState = lifecycleState_;
        lifecycleState_ = state;
        if (trigger != RecoveryTrigger::Unknown) {
          lastRecoveryTrigger_ = trigger;
        }
        lastRecoveryAction_ = action;
        callback = recoveryEventCallback_;
        report.previousState = previousState;
        report.state = state;
        report.trigger = trigger != RecoveryTrigger::Unknown ? trigger : lastRecoveryTrigger_;
        report.action = action;
        report.terminalManualShutdown = terminalManualShutdown_;
        report.recoveryPending = recoveryPending_;
        report.cooldownDelayMs = cooldownDelayMs;
        report.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemainingLocked().count());
        report.sessionId = currentSessionId;
        report.previousSessionId = lastClosedSessionId;
        report.monotonicMs = MonotonicNowMs64();
      }
      if (callback) {
        callback(report);
      }
    }

    void StartRecovery(RecoveryTrigger trigger,
                       SessionOpenReason openReason,
                       uint32_t delayMs,
                       uint64_t currentSessionId,
                       uint64_t lastClosedSessionId) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        nextSessionOpenReason_ = openReason;
        nextSessionOpenTrigger_ = trigger;
        nextSessionOpenDelayMs_ = delayMs;
        recoveryPending_ = true;
        cooldownUntilMs_ = MonotonicNowMs64() + delayMs;
      }
      TransitionLifecycle(delayMs == 0 ? ClientLifecycleState::Recovering
                                       : ClientLifecycleState::Cooldown,
                          trigger, RecoveryAction::Restart, delayMs,
                          currentSessionId, lastClosedSessionId);
      NotifyWaiters();
    }

    void EnterDemandReconnect(RecoveryAction lastAction,
                              uint64_t currentSessionId,
                              uint64_t lastClosedSessionId) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        nextSessionOpenReason_ = SessionOpenReason::DemandReconnect;
        nextSessionOpenTrigger_ = RecoveryTrigger::DemandReconnect;
        nextSessionOpenDelayMs_ = 0;
        recoveryPending_ = true;
      }
      TransitionLifecycle(ClientLifecycleState::Recovering, RecoveryTrigger::DemandReconnect,
                          lastAction, 0, currentSessionId, lastClosedSessionId);
    }

    void EnterRecovering(RecoveryAction action,
                         uint64_t currentSessionId,
                         uint64_t lastClosedSessionId) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        recoveryPending_ = true;
      }
      TransitionLifecycle(ClientLifecycleState::Recovering, RecoveryTrigger::Unknown,
                          action, 0, currentSessionId, lastClosedSessionId);
    }

    void EnterDisconnected(RecoveryTrigger trigger,
                           uint64_t currentSessionId,
                           uint64_t lastClosedSessionId) {
      ClearRecoveryWindow();
      TransitionLifecycle(ClientLifecycleState::Disconnected, trigger, RecoveryAction::Ignore, 0,
                          currentSessionId, lastClosedSessionId);
      NotifyWaiters();
    }

    void EnterIdleClosed(uint64_t currentSessionId, uint64_t lastClosedSessionId) {
      ClearRecoveryWindow();
      TransitionLifecycle(ClientLifecycleState::IdleClosed, RecoveryTrigger::IdlePolicy,
                          RecoveryAction::IdleClose, 0, currentSessionId, lastClosedSessionId);
      NotifyWaiters();
    }

    void EnterTerminalClosed(uint64_t currentSessionId, uint64_t lastClosedSessionId) {
      ClearRecoveryWindow();
      SetTerminalManualShutdown();
      TransitionLifecycle(ClientLifecycleState::Closed, RecoveryTrigger::ManualShutdown,
                          RecoveryAction::ManualShutdown, 0,
                          currentSessionId, lastClosedSessionId);
      NotifyWaiters();
    }

    [[nodiscard]] bool CooldownActive() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return MonotonicNowMs64() < cooldownUntilMs_;
    }

    [[nodiscard]] std::chrono::milliseconds CooldownRemaining() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return CooldownRemainingLocked();
    }

    [[nodiscard]] bool RecoveryPending() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return recoveryPending_;
    }

    [[nodiscard]] RecoveryTrigger PendingSessionOpenTrigger() const {
      std::lock_guard<std::mutex> lock(mutex_);
      if (nextSessionOpenTrigger_ != RecoveryTrigger::Unknown) {
        return nextSessionOpenTrigger_;
      }
      if (nextSessionOpenReason_ == SessionOpenReason::DemandReconnect) {
        return RecoveryTrigger::DemandReconnect;
      }
      return lastRecoveryTrigger_;
    }

    [[nodiscard]] RecoveryAction LastRecoveryAction() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return lastRecoveryAction_;
    }

    void NotifyWaiters() {
      cv_.notify_all();
    }

    StatusCode WaitForRecovery(std::chrono::steady_clock::time_point deadline,
                               const std::atomic<bool>& running,
                               const std::atomic<bool>& clientClosed) {
      std::unique_lock<std::mutex> lock(mutex_);
      while (running.load(std::memory_order_acquire)) {
        if (clientClosed.load(std::memory_order_acquire)) {
          return StatusCode::ClientClosed;
        }
        const uint64_t nowMs = MonotonicNowMs64();
        if (nowMs >= cooldownUntilMs_) {
          return StatusCode::Ok;
        }
        const auto now = std::chrono::steady_clock::now();
        if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
          HILOGW("RpcClient::WaitForRecovery timed out");
          return StatusCode::CooldownActive;
        }
        auto wakeAt = now + std::chrono::milliseconds(cooldownUntilMs_ - nowMs);
        if (deadline != std::chrono::steady_clock::time_point::max()) {
          wakeAt = std::min(wakeAt, deadline);
        }
        cv_.wait_until(lock, wakeAt);
      }
      HILOGW("RpcClient::WaitForRecovery aborted because client stopped");
      return StatusCode::PeerDisconnected;
    }

    StatusCode WaitForRecoveryRetry(std::chrono::steady_clock::time_point deadline,
                                    const std::atomic<bool>& running,
                                    const std::atomic<bool>& clientClosed) {
      std::unique_lock<std::mutex> lock(mutex_);
      while (running.load(std::memory_order_acquire)) {
        if (clientClosed.load(std::memory_order_acquire)) {
          return StatusCode::ClientClosed;
        }
        if (!recoveryPending_) {
          return StatusCode::PeerDisconnected;
        }
        const auto now = std::chrono::steady_clock::now();
        if (deadline != std::chrono::steady_clock::time_point::max() && now >= deadline) {
          HILOGW("RpcClient::WaitForRecoveryRetry timed out");
          return StatusCode::PeerDisconnected;
        }
        auto waitFor = kRecoveryRetryPollPeriod;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
          waitFor = std::min(waitFor,
                             std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        }
        if (waitFor <= std::chrono::milliseconds::zero()) {
          HILOGW("RpcClient::WaitForRecoveryRetry reached zero wait budget");
          return StatusCode::PeerDisconnected;
        }
        cv_.wait_for(lock, waitFor);
        return StatusCode::Ok;
      }
      HILOGW("RpcClient::WaitForRecoveryRetry aborted because client stopped");
      return StatusCode::PeerDisconnected;
    }

    RecoveryRuntimeSnapshot GetSnapshot(uint64_t currentSessionId, uint64_t lastClosedSessionId) const {
      std::lock_guard<std::mutex> lock(mutex_);
      RecoveryRuntimeSnapshot snapshot;
      snapshot.lifecycleState = lifecycleState_;
      snapshot.lastTrigger = lastRecoveryTrigger_;
      snapshot.lastRecoveryAction = lastRecoveryAction_;
      snapshot.recoveryPending = recoveryPending_;
      snapshot.terminalManualShutdown = terminalManualShutdown_;
      snapshot.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemainingLocked().count());
      snapshot.currentSessionId = currentSessionId;
      snapshot.lastOpenedSessionId = lastOpenedSessionId_;
      snapshot.lastClosedSessionId = lastClosedSessionId;
      return snapshot;
    }

   private:
    std::chrono::milliseconds CooldownRemainingLocked() const {
      return ::MemRpc::CooldownRemaining(cooldownUntilMs_);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    RecoveryEventCallback recoveryEventCallback_;
    bool recoveryPending_ = false;
    ClientLifecycleState lifecycleState_ = ClientLifecycleState::Uninitialized;
    uint64_t cooldownUntilMs_ = 0;
    SessionOpenReason nextSessionOpenReason_ = SessionOpenReason::InitialInit;
    RecoveryTrigger nextSessionOpenTrigger_ = RecoveryTrigger::Unknown;
    uint32_t nextSessionOpenDelayMs_ = 0;
    RecoveryTrigger lastRecoveryTrigger_ = RecoveryTrigger::Unknown;
    RecoveryAction lastRecoveryAction_ = RecoveryAction::Ignore;
    bool terminalManualShutdown_ = false;
    uint64_t lastOpenedSessionId_ = 0;
  };

  class SubmitWorker {
   public:
    explicit SubmitWorker(Impl& owner) : owner_(owner) {}

    void Run() {
      while (owner_.running_.load(std::memory_order_acquire)) {
        PendingSubmit submit;
        {
          std::unique_lock<std::mutex> lock(owner_.submitMutex_);
          owner_.submitCv_.wait(lock, [this] {
            return !owner_.running_.load(std::memory_order_acquire) || !owner_.submitQueue_.empty();
          });
          if (!owner_.running_.load(std::memory_order_acquire) && owner_.submitQueue_.empty()) {
            break;
          }
          submit = std::move(owner_.submitQueue_.front());
          owner_.submitQueue_.pop_front();
        }
        SubmitOne(submit);
      }
    }

   private:
    SubmitConfig MakeSubmitConfig(const PendingSubmit& submit) const {
      SubmitConfig config;
      config.infiniteWait = submit.call.admissionTimeoutMs == 0;
      config.waitForRecovery = submit.call.waitForRecovery;
      if (!config.infiniteWait) {
        config.deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(submit.call.admissionTimeoutMs);
      }
      if (config.waitForRecovery && submit.call.recoveryTimeoutMs != 0) {
        config.recoveryDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(submit.call.recoveryTimeoutMs);
      }
      return config;
    }

    SubmitAction HandleSessionStatus(const PendingSubmit& submit,
                                     const PendingInfo& info,
                                     const SubmitConfig& config,
                                     StatusCode sessionStatus) {
      if (sessionStatus == StatusCode::Ok) {
        return SubmitAction::Proceed;
      }
      if (sessionStatus == StatusCode::CooldownActive && config.waitForRecovery) {
        const StatusCode waitStatus = owner_.WaitForRecovery(config.recoveryDeadline);
        if (waitStatus == StatusCode::Ok) {
          return SubmitAction::Retry;
        }
        HILOGE("RpcClient::SubmitOne recovery wait failed: request_id=%{public}llu opcode=%{public}u status=%{public}d",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode,
               static_cast<int>(waitStatus));
        owner_.FailAndResolve(info, waitStatus, FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      if (sessionStatus == StatusCode::PeerDisconnected && config.waitForRecovery) {
        const StatusCode waitStatus = owner_.WaitForRecoveryRetry(config.recoveryDeadline);
        if (waitStatus == StatusCode::Ok) {
          return SubmitAction::Retry;
        }
        HILOGE("RpcClient::SubmitOne recovery retry failed: request_id=%{public}llu opcode=%{public}u status=%{public}d",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode,
               static_cast<int>(waitStatus));
        owner_.FailAndResolve(info, waitStatus, FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      HILOGE("RpcClient::SubmitOne session unavailable: request_id=%{public}llu opcode=%{public}u status=%{public}d",
             static_cast<unsigned long long>(submit.requestId), submit.call.opcode,
             static_cast<int>(sessionStatus));
      owner_.FailAndResolve(info, sessionStatus, FailureStage::Admission, submit.future);
      return SubmitAction::Finish;
    }

    SubmitAction HandleDisconnectedPush(const PendingSubmit& submit,
                                        const PendingInfo& info,
                                        const SubmitConfig& config) {
      owner_.CloseLiveSession();
      if (!config.infiniteWait && DeadlineReached(config.deadline)) {
        HILOGE("RpcClient::SubmitOne timed out after disconnect: request_id=%{public}llu opcode=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
        owner_.FailAndResolve(info, StatusCode::QueueTimeout, FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return SubmitAction::Retry;
    }

    SubmitAction HandleQueueFullPush(const PendingSubmit& submit,
                                     const PendingInfo& info,
                                     const SubmitConfig& config) {
      if (!config.infiniteWait && DeadlineReached(config.deadline)) {
        HILOGE("RpcClient::SubmitOne admission deadline reached: request_id=%{public}llu opcode=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
        owner_.FailAndResolve(info, StatusCode::QueueTimeout, FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      const auto waitResult = owner_.WaitForRequestCredit(config.deadline);
      if (waitResult == PollEventFdResult::Ready || waitResult == PollEventFdResult::Retry) {
        return SubmitAction::Retry;
      }
      if (waitResult == PollEventFdResult::Timeout) {
        HILOGE("RpcClient::SubmitOne request credit wait timed out: request_id=%{public}llu opcode=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
        owner_.FailAndResolve(info,
                              config.infiniteWait ? StatusCode::QueueFull : StatusCode::QueueTimeout,
                              FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      HILOGE("RpcClient::SubmitOne request credit wait failed: request_id=%{public}llu opcode=%{public}u",
             static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
      owner_.CloseLiveSession();
      return SubmitAction::Retry;
    }

    SubmitAction HandlePushStatus(const PendingSubmit& submit,
                                  const PendingInfo& info,
                                  const SubmitConfig& config,
                                  StatusCode pushStatus) {
      if (pushStatus == StatusCode::Ok) {
        return SubmitAction::Finish;
      }
      if (pushStatus == StatusCode::PayloadTooLarge) {
        HILOGE("RpcClient::SubmitOne payload rejected: request_id=%{public}llu opcode=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
        owner_.FailAndResolve(info, pushStatus, FailureStage::Admission, submit.future);
        return SubmitAction::Finish;
      }
      if (pushStatus == StatusCode::PeerDisconnected) {
        return HandleDisconnectedPush(submit, info, config);
      }
      if (pushStatus == StatusCode::QueueFull) {
        return HandleQueueFullPush(submit, info, config);
      }
      HILOGE("RpcClient::SubmitOne push failed: request_id=%{public}llu opcode=%{public}u status=%{public}d",
             static_cast<unsigned long long>(submit.requestId), submit.call.opcode,
             static_cast<int>(pushStatus));
      owner_.FailAndResolve(info, pushStatus, FailureStage::Admission, submit.future);
      return SubmitAction::Finish;
    }

    void SubmitOne(const PendingSubmit& submit) {
      const SubmitConfig config = MakeSubmitConfig(submit);
      PendingInfo info = owner_.MakePendingInfo(submit);
      while (owner_.running_.load(std::memory_order_acquire)) {
        const SubmitAction sessionAction =
            HandleSessionStatus(submit, info, config, owner_.EnsureLiveSession());
        if (sessionAction == SubmitAction::Retry) {
          continue;
        }
        if (sessionAction == SubmitAction::Finish) {
          return;
        }
        const SubmitAction pushAction = HandlePushStatus(submit, info, config, owner_.TryPushRequest(submit));
        if (pushAction == SubmitAction::Retry) {
          continue;
        }
        return;
      }
      HILOGE("RpcClient::SubmitOne aborted because client stopped: request_id=%{public}llu opcode=%{public}u",
             static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
      owner_.FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission, submit.future);
    }

    Impl& owner_;
  };

  class ResponseWorker {
   public:
    explicit ResponseWorker(Impl& owner) : owner_(owner) {}

    void Run() {
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

      while (owner_.running_.load(std::memory_order_acquire)) {
        if (DrainResponseRing()) {
          continue;
        }

        const auto snapshot = owner_.LoadSessionSnapshot();
        const uint64_t currentSessionId = snapshot->sessionId;
        int nextPollFd = -1;
        bool sessionChanged = false;
        if (currentSessionId != activePollSessionId) {
          owner_.sessionController_.WithSessionLocked([&](Session& session) {
            if (session.Valid()) {
              const uint64_t lockedSessionId = owner_.LoadSessionSnapshot()->sessionId;
              if (lockedSessionId != activePollSessionId) {
                sessionChanged = true;
                const int responseFd = session.Handles().respEventFd;
                if (responseFd >= 0) {
                  nextPollFd = dup(responseFd);
                  if (nextPollFd < 0) {
                    HILOGE("RpcClient::ResponseLoop dup failed: responseFd=%{public}d", responseFd);
                  }
                }
              }
            }
          });
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
          HILOGE("RpcClient::ResponseLoop detected response poll failure session_id=%{public}llu",
                 static_cast<unsigned long long>(failedSessionId));
          resetActivePollFd();
          owner_.HandleEngineDeath(failedSessionId);
        }
      }
    }

   private:
    void ResolveCompletedRequest(const ResponseRingEntry& entry) {
      std::optional<PendingRequest> pending = owner_.pendingStore_.Take(entry.requestId);
      if (!pending.has_value()) {
        HILOGW("RpcClient::ResolveCompletedRequest ignored late reply request_id=%{public}llu",
               static_cast<unsigned long long>(entry.requestId));
        return;
      }
      RpcReply reply;
      reply.status = static_cast<StatusCode>(entry.statusCode);
      reply.errorCode = entry.errorCode;
      reply.payload.assign(entry.payload.begin(), entry.payload.begin() + entry.resultSize);
      if (reply.status != StatusCode::Ok) {
        owner_.ApplyFailureRecoveryDecision(
            pending->info, reply.status, FailureStageForStatus(reply.status));
      }
      owner_.ResolveState(pending->future, std::move(reply));
      owner_.TouchActivity();
    }

    void DeliverEvent(const ResponseRingEntry& entry) {
      RpcEventCallback callback;
      {
        std::lock_guard<std::mutex> lock(owner_.eventMutex_);
        callback = owner_.eventCallback_;
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
      owner_.TouchActivity();
    }

    bool DrainResponseRing() {
      bool drained = false;
      while (true) {
        ResponseRingEntry entry;
        bool ringBecameNotFull = false;
        int respCreditEventFd = -1;
        const bool popped = owner_.sessionController_.WithSessionLocked([&](Session& session) {
          if (!session.Valid() || session.Header() == nullptr) {
            return false;
          }
          const RingCursor& cursor = session.Header()->responseRing;
          ringBecameNotFull = cursor.capacity != 0 && RingCount(cursor) == cursor.capacity;
          if (!session.PopResponse(&entry)) {
            return false;
          }
          respCreditEventFd = session.Handles().respCreditEventFd;
          return true;
        });
        if (!popped) {
          return drained;
        }
        drained = true;
        if (ringBecameNotFull) {
          (void)SignalEventFd(respCreditEventFd);
        }
        if (entry.resultSize > ResponseRingEntry::INLINE_PAYLOAD_BYTES) {
          HILOGE("RpcClient::DrainResponseRing detected oversized response request_id=%{public}llu result_size=%{public}u",
                 static_cast<unsigned long long>(entry.requestId), entry.resultSize);
          owner_.HandleEngineDeath(owner_.CurrentSessionId());
          return true;
        }
        if (entry.messageKind == ResponseMessageKind::Event) {
          DeliverEvent(entry);
        } else {
          ResolveCompletedRequest(entry);
        }
      }
    }

    Impl& owner_;
  };

  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap)
      : sessionController_(std::move(bootstrap)) {
    sessionController_.InstallDeathCallback(
        [this](uint64_t sessionId) { HandleEngineDeath(sessionId); });
  }

  // Component ownership: session transport, recovery state, request stores, and
  // worker threads each have a dedicated helper. Impl keeps only the shared
  // orchestration and cross-component glue.
  SessionController sessionController_;
  mutable std::mutex submitMutex_;
  mutable std::mutex eventMutex_;
  mutable std::mutex sessionReadyMutex_;
  mutable std::mutex watchdogMutex_;
  RecoveryCoordinator recoveryCoordinator_;
  mutable std::mutex recoveryPolicyMutex_;
  SubmitWorker submitWorker_{*this};
  ResponseWorker responseWorker_{*this};
  RecoveryPolicy recoveryPolicy_;
  RpcEventCallback eventCallback_;
  SessionReadyCallback sessionReadyCallback_;
  std::deque<PendingSubmit> submitQueue_;
  PendingRequestStore pendingStore_;
  std::condition_variable submitCv_;
  std::condition_variable watchdogCv_;
  std::thread submitThread_;
  std::thread responseThread_;
  std::thread watchdogThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shuttingDown{false};
  std::atomic<bool> clientClosed_{false};
  std::atomic<bool> submitterWaitingForCredit_{false};
  std::atomic<uint64_t> nextRequestId_{1};
  std::atomic<uint64_t> lastActivityMs_{0};
  std::atomic<uint64_t> lastClosedSessionId_{0};
  std::atomic<uint32_t> sessionGeneration_{0};

  static RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(state);
  }

  // Shared state snapshots and callback plumbing.
  void TouchActivity() {
    lastActivityMs_.store(MonotonicNowMs64(), std::memory_order_release);
  }

  std::shared_ptr<const SessionSnapshot> LoadSessionSnapshot() const {
    return sessionController_.LoadSnapshot();
  }

  uint64_t CurrentSessionId() const {
    return sessionController_.CurrentSessionId();
  }

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
    sessionController_.SetBootstrapChannel(
        std::move(bootstrap), [this](uint64_t sessionId) { HandleEngineDeath(sessionId); });
  }

  void SetSessionReadyCallback(SessionReadyCallback callback) {
    std::lock_guard<std::mutex> lock(sessionReadyMutex_);
    sessionReadyCallback_ = std::move(callback);
  }

  void SetRecoveryEventCallback(RecoveryEventCallback callback) {
    recoveryCoordinator_.SetRecoveryEventCallback(std::move(callback));
  }

  ClientLifecycleState LifecycleState() const {
    return recoveryCoordinator_.LifecycleState();
  }

  void TransitionLifecycle(ClientLifecycleState state,
                           RecoveryTrigger trigger,
                           RecoveryAction action,
                           uint32_t cooldownDelayMs = 0) {
    recoveryCoordinator_.TransitionLifecycle(
        state, trigger, action, cooldownDelayMs,
        CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
  }

  void MarkNextSessionOpen(SessionOpenReason reason, uint32_t delayMs) {
    recoveryCoordinator_.MarkNextSessionOpen(reason, delayMs);
  }

  void MarkNextSessionOpen(SessionOpenReason reason, RecoveryTrigger trigger, uint32_t delayMs) {
    recoveryCoordinator_.MarkNextSessionOpen(reason, trigger, delayMs);
  }

  void PrepareForInitOpen() {
    recoveryCoordinator_.PrepareForInitOpen(
        sessionGeneration_.load(std::memory_order_acquire) == 0);
  }

  void NotifySessionReady(uint64_t sessionId) {
    SessionReadyCallback callback;
    {
      std::lock_guard<std::mutex> lock(sessionReadyMutex_);
      callback = sessionReadyCallback_;
    }

    SessionReadyReport report;
    const auto readyState = recoveryCoordinator_.ConsumeSessionReady(sessionId);
    report.reason = readyState.reason;
    report.scheduledDelayMs = readyState.scheduledDelayMs;
    report.sessionId = sessionId;
    report.previousSessionId = lastClosedSessionId_.load(std::memory_order_acquire);
    report.generation = sessionGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    report.monotonicMs = MonotonicNowMs64();
    TransitionLifecycle(ClientLifecycleState::Active, readyState.trigger,
                        readyState.lastAction, readyState.scheduledDelayMs);

    if (callback) {
      callback(report);
    }
    NotifyRecoveryWaiters();
  }

  RecoveryPolicy LoadRecoveryPolicy() const {
    std::lock_guard<std::mutex> lock(recoveryPolicyMutex_);
    return recoveryPolicy_;
  }

  bool IsStaleRecoverySignal(uint64_t observedSessionId) const {
    if (observedSessionId == 0) {
      return false;
    }
    const uint64_t currentSessionId = CurrentSessionId();
    return currentSessionId != 0 && observedSessionId != currentSessionId;
  }

  void ClearRecoveryWindow() {
    recoveryCoordinator_.ClearRecoveryWindow();
  }

  void NotifyRecoveryWaiters() {
    recoveryCoordinator_.NotifyWaiters();
  }

  // Shared reply resolution and recovery-decision helpers.
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

  RpcFailure BuildFailureReport(const PendingInfo& info, StatusCode status, FailureStage stage) const {
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
    return failure;
  }

  void ApplyFailureRecoveryDecision(const PendingInfo& info, StatusCode status, FailureStage stage) {
    const RecoveryPolicy policy = LoadRecoveryPolicy();
    if (!policy.onFailure) {
      return;
    }

    const RpcFailure failure = BuildFailureReport(info, status, stage);
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
    ApplyFailureRecoveryDecision(info, status, stage);
    RpcReply reply;
    reply.status = status;
    ResolveState(future, std::move(reply));
  }

  void FailAllPending(StatusCode status) {
    std::vector<PendingRequest> toFail = pendingStore_.TakeAll();
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

  // Lifecycle and session orchestration.
  StatusCode OpenSession() {
    const StatusCode status = sessionController_.OpenSession();
    if (status != StatusCode::Ok) {
      return status;
    }
    TouchActivity();
    const uint64_t sessionId = CurrentSessionId();
    NotifySessionReady(sessionId);
    return StatusCode::Ok;
  }

  void CloseLiveSession() {
    const uint64_t closedSessionId = sessionController_.CloseLiveSession();
    if (closedSessionId != 0) {
      lastClosedSessionId_.store(closedSessionId, std::memory_order_release);
    }
  }

  void EnterIdleClosed() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    ClearRecoveryWindow();
    CloseLiveSession();
    recoveryCoordinator_.EnterIdleClosed(
        CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
  }

  void EnterDisconnected(RecoveryTrigger trigger) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    recoveryCoordinator_.EnterDisconnected(
        trigger, CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
  }

  void EnterTerminalClosed() {
    clientClosed_.store(true, std::memory_order_release);
    shuttingDown.store(true, std::memory_order_release);
    CloseLiveSession();
    recoveryCoordinator_.EnterTerminalClosed(
        CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
  }

  void ScheduleRecovery(RecoveryTrigger trigger,
                        SessionOpenReason openReason,
                        uint32_t delayMs,
                        bool fromEngineDeath) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    recoveryCoordinator_.StartRecovery(
        trigger, openReason, delayMs,
        CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
    CloseLiveSession();
    if (!fromEngineDeath) {
      FailAllPending(StatusCode::PeerDisconnected);
    }
    if (delayMs == 0) {
      (void)EnsureLiveSession();
    }
  }

  bool CooldownActive() const {
    return recoveryCoordinator_.CooldownActive();
  }

  std::chrono::milliseconds CooldownRemaining() const {
    return recoveryCoordinator_.CooldownRemaining();
  }

  bool RecoveryPending() const {
    return recoveryCoordinator_.RecoveryPending();
  }

  RecoveryTrigger PendingSessionOpenTrigger() const {
    return recoveryCoordinator_.PendingSessionOpenTrigger();
  }

  StatusCode EnsureLiveSession() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      HILOGW("RpcClient::EnsureLiveSession rejected: client already closed");
      return StatusCode::ClientClosed;
    }
    if (CooldownActive()) {
      HILOGW("RpcClient::EnsureLiveSession delayed by cooldown remaining_ms=%{public}lld",
             static_cast<long long>(CooldownRemaining().count()));
      return StatusCode::CooldownActive;
    }
    if (sessionController_.HasLiveSession()) {
      return StatusCode::Ok;
    }
    ClientLifecycleState lifecycleState = LifecycleState();
    const RecoveryAction lastAction = recoveryCoordinator_.LastRecoveryAction();
    if (lifecycleState == ClientLifecycleState::IdleClosed ||
        lifecycleState == ClientLifecycleState::Disconnected) {
      recoveryCoordinator_.EnterDemandReconnect(
          RecoveryAction::Ignore,
          CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
    } else if (lifecycleState != ClientLifecycleState::Recovering &&
               lifecycleState != ClientLifecycleState::Uninitialized) {
      recoveryCoordinator_.EnterRecovering(
          lastAction, CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
    }
    const StatusCode status = OpenSession();
    if (status != StatusCode::Ok) {
      const RecoveryTrigger failureTrigger = PendingSessionOpenTrigger();
      lifecycleState = LifecycleState();
      if (RecoveryPending() || lifecycleState == ClientLifecycleState::Recovering ||
          lifecycleState == ClientLifecycleState::Cooldown) {
        HILOGW("RpcClient::EnsureLiveSession abandoning recovery after open failure: status=%{public}d trigger=%{public}d",
               static_cast<int>(status), static_cast<int>(failureTrigger));
        EnterDisconnected(failureTrigger);
      }
      HILOGE("RpcClient::EnsureLiveSession failed to open session: status=%{public}d lifecycle=%{public}d",
             static_cast<int>(status), static_cast<int>(LifecycleState()));
    }
    return status;
  }

  void StartThreads() {
    running_.store(true, std::memory_order_release);
    shuttingDown.store(false, std::memory_order_release);
    submitThread_ = std::thread([this] { submitWorker_.Run(); });
    responseThread_ = std::thread([this] { responseWorker_.Run(); });
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
    NotifyRecoveryWaiters();
    const auto snapshot = sessionController_.LoadSnapshot();
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
    {
      std::lock_guard<std::mutex> lock(recoveryPolicyMutex_);
      recoveryPolicy_ = {};
    }
    recoveryCoordinator_.ClearRecoveryEventCallback();
    sessionController_.ClearDeathCallback();
    {
      std::lock_guard<std::mutex> lock(sessionReadyMutex_);
      sessionReadyCallback_ = {};
    }
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

  // Submission primitives shared by InvokeAsync and SubmitWorker.
  PollEventFdResult WaitForRequestCredit(std::chrono::steady_clock::time_point deadline) {
    const int fd = LoadSessionSnapshot()->reqCreditEventFd;
    if (fd < 0) {
      HILOGE("RpcClient::WaitForRequestCredit failed: invalid reqCreditEventFd=%{public}d", fd);
      return PollEventFdResult::Failed;
    }

    submitterWaitingForCredit_.store(true, std::memory_order_release);
    [[maybe_unused]] const auto clearWaiting =
        MakeScopeExit([this] { submitterWaitingForCredit_.store(false, std::memory_order_release); });

    pollfd pollFd{fd, POLLIN, 0};
    while (running_.load(std::memory_order_acquire)) {
      const int64_t remainingMs = RemainingTimeoutMs(deadline);
      if (remainingMs <= 0) {
        HILOGW("RpcClient::WaitForRequestCredit timed out waiting for request credit");
        return PollEventFdResult::Timeout;
      }
      const auto waitResult = PollEventFd(&pollFd, static_cast<int>(remainingMs));
      if (waitResult == PollEventFdResult::Retry) {
        continue;
      }
      if (waitResult == PollEventFdResult::Failed) {
        HILOGE("RpcClient::WaitForRequestCredit poll failed fd=%{public}d", fd);
      }
      return waitResult;
    }
    HILOGW("RpcClient::WaitForRequestCredit aborted because client stopped");
    return PollEventFdResult::Failed;
  }

  StatusCode WaitForRecovery(std::chrono::steady_clock::time_point deadline) {
    return recoveryCoordinator_.WaitForRecovery(deadline, running_, clientClosed_);
  }

  StatusCode WaitForRecoveryRetry(std::chrono::steady_clock::time_point deadline) {
    return recoveryCoordinator_.WaitForRecoveryRetry(deadline, running_, clientClosed_);
  }

  StatusCode TryPushRequest(const PendingSubmit& submit) {
    return sessionController_.WithSessionLocked([&](Session& session) -> StatusCode {
      if (!session.Valid() || session.Header() == nullptr) {
        HILOGE("RpcClient::TryPushRequest failed: session not valid request_id=%{public}llu opcode=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.opcode);
        return StatusCode::PeerDisconnected;
      }
      if (submit.call.payload.size() > session.Header()->maxRequestBytes ||
          submit.call.payload.size() > RequestRingEntry::INLINE_PAYLOAD_BYTES) {
        HILOGE("RpcClient::TryPushRequest failed: payload too large request_id=%{public}llu payload_size=%{public}zu max=%{public}u inline_max=%{public}u",
               static_cast<unsigned long long>(submit.requestId), submit.call.payload.size(),
               session.Header()->maxRequestBytes, RequestRingEntry::INLINE_PAYLOAD_BYTES);
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
      pendingStore_.Put(submit.requestId, std::move(pending));

      const bool highPriority = IsHighPriority(submit.call);
      const StatusCode status =
          session.PushRequest(highPriority ? QueueKind::HighRequest : QueueKind::NormalRequest, entry);
      if (status != StatusCode::Ok) {
        pendingStore_.Erase(submit.requestId);
        if (status != StatusCode::QueueFull) {
          HILOGE("RpcClient::TryPushRequest failed: status=%{public}d request_id=%{public}llu opcode=%{public}u",
                 static_cast<int>(status), static_cast<unsigned long long>(submit.requestId),
                 submit.call.opcode);
        }
        return status;
      }

      const RingCursor& cursor =
          highPriority ? session.Header()->highRing : session.Header()->normalRing;
      if (RingCountIsOneAfterPush(cursor)) {
        const int fd = highPriority ? session.Handles().highReqEventFd : session.Handles().normalReqEventFd;
        if (!SignalEventFd(fd)) {
          HILOGW("RpcClient::TryPushRequest failed to signal request event fd=%{public}d request_id=%{public}llu",
                 fd, static_cast<unsigned long long>(submit.requestId));
        }
      }
      TouchActivity();
      return StatusCode::Ok;
    });
  }

  // Watchdog-driven monitoring and recovery-decision hooks.
  void MaybeRunPendingTimeouts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingRequest> expired = pendingStore_.TakeExpired(now);
    for (auto& pending : expired) {
      FailAndResolve(pending.info, StatusCode::ExecTimeout, FailureStage::Timeout, pending.future);
    }
  }

  void ApplyIdleRecoveryDecision(const RecoveryPolicy& policy, uint64_t idleMs) {
    const RecoveryDecision decision = policy.onIdle(idleMs);
    ApplyRecoveryDecision(decision, RecoveryTrigger::IdlePolicy, false);
  }

  void MaybeRunIdlePolicy() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    const RecoveryPolicy policy = LoadRecoveryPolicy();
    if (!policy.onIdle) {
      return;
    }
    if (!pendingStore_.Empty()) {
      return;
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
    ApplyIdleRecoveryDecision(policy, idleMs);
  }

  std::optional<ExternalRecoverySignal> ToExternalRecoverySignal(ChannelHealthStatus status) const {
    switch (status) {
      case ChannelHealthStatus::Timeout:
        return ExternalRecoverySignal::ChannelHealthTimeout;
      case ChannelHealthStatus::Malformed:
        return ExternalRecoverySignal::ChannelHealthMalformed;
      case ChannelHealthStatus::Unhealthy:
        return ExternalRecoverySignal::ChannelHealthUnhealthy;
      case ChannelHealthStatus::SessionMismatch:
        return ExternalRecoverySignal::ChannelHealthSessionMismatch;
      case ChannelHealthStatus::Healthy:
      case ChannelHealthStatus::Unsupported:
        return std::nullopt;
    }
    return std::nullopt;
  }

  void RequestHealthCheckRecovery(ChannelHealthStatus status, uint64_t expectedSessionId) {
    const std::optional<ExternalRecoverySignal> signal = ToExternalRecoverySignal(status);
    if (!signal.has_value()) {
      return;
    }

    HILOGE("RpcClient::MaybeRunHealthCheck status=%{public}d session_id=%{public}llu",
           static_cast<int>(status), static_cast<unsigned long long>(expectedSessionId));
    RequestExternalRecovery({*signal, expectedSessionId, 0});
  }

  void MaybeRunHealthCheck() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    std::shared_ptr<IBootstrapChannel> bootstrap = sessionController_.LoadBootstrap();
    if (bootstrap == nullptr) {
      return;
    }
    const uint64_t expectedSessionId = CurrentSessionId();
    if (expectedSessionId == 0) {
      return;
    }

    const ChannelHealthResult result = bootstrap->CheckHealth(expectedSessionId);
    RequestHealthCheckRecovery(result.status, expectedSessionId);
  }

  EngineDeathReport BuildEngineDeathReport(uint64_t observedSessionId) const {
    EngineDeathReport report;
    const uint64_t currentSessionId = CurrentSessionId();
    report.deadSessionId = observedSessionId == 0 ? currentSessionId : observedSessionId;
    return report;
  }

  void ApplyEngineDeathRecoveryDecision(const EngineDeathReport& report) {
    const RecoveryPolicy policy = LoadRecoveryPolicy();
    if (!policy.onEngineDeath) {
      HILOGE("RpcClient::HandleEngineDeath has no recovery policy session_id=%{public}llu",
             static_cast<unsigned long long>(report.deadSessionId));
      EnterDisconnected(RecoveryTrigger::EngineDeath);
      return;
    }

    const RecoveryDecision decision = policy.onEngineDeath(report);
    if (decision.action == RecoveryAction::Ignore) {
      HILOGW("RpcClient::HandleEngineDeath policy ignored recovery dead_session_id=%{public}llu",
             static_cast<unsigned long long>(report.deadSessionId));
      EnterDisconnected(RecoveryTrigger::EngineDeath);
      return;
    }
    ApplyRecoveryDecision(decision, RecoveryTrigger::EngineDeath, true);
  }

  void HandleEngineDeathDetection(uint64_t sessionId) {
    const EngineDeathReport report = BuildEngineDeathReport(sessionId);
    CloseLiveSession();
    ClearRecoveryWindow();
    FailAllPending(StatusCode::CrashedDuringExecution);
    ApplyEngineDeathRecoveryDecision(report);
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
    if (IsStaleRecoverySignal(sessionId)) {
      HILOGW("RpcClient::HandleEngineDeath ignored stale session_id=%{public}llu current_session_id=%{public}llu",
             static_cast<unsigned long long>(sessionId),
             static_cast<unsigned long long>(current));
      return;
    }
    HandleEngineDeathDetection(sessionId);
  }

  void RequestExternalRecovery(ExternalRecoveryRequest request) {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return;
    }
    const uint64_t current = CurrentSessionId();
    if (IsStaleRecoverySignal(request.sessionId)) {
      HILOGW("RpcClient::RequestExternalRecovery ignored stale request session_id=%{public}llu current_session_id=%{public}llu",
             static_cast<unsigned long long>(request.sessionId),
             static_cast<unsigned long long>(current));
      return;
    }
    HILOGW("RpcClient::RequestExternalRecovery requested signal=%{public}d session_id=%{public}llu delay_ms=%{public}u",
           static_cast<int>(request.signal), static_cast<unsigned long long>(request.sessionId),
           request.delayMs);
    ScheduleRecovery(RecoveryTrigger::ExternalHealthSignal,
                     SessionOpenReason::ExternalRecovery,
                     request.delayMs, false);
  }

  // Public-operation entrypoints and runtime introspection.
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
        HILOGE("RpcClient::InvokeAsync rejected because client not running opcode=%{public}u status=%{public}d",
               call.opcode, static_cast<int>(status));
        ApplyFailureRecoveryDecision(info, status, FailureStage::Admission);
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
    stats.pendingCalls = pendingStore_.Size();
    {
      sessionController_.WithSessionLocked([&](const Session& session) {
        if (session.Header() != nullptr) {
          stats.highRequestRingPending = RingCount(session.Header()->highRing);
          stats.normalRequestRingPending = RingCount(session.Header()->normalRing);
          stats.responseRingPending = RingCount(session.Header()->responseRing);
        }
      });
    }
    stats.waitingForRequestCredit = submitterWaitingForCredit_.load(std::memory_order_acquire);
    stats.recoveryPending = RecoveryPending();
    stats.cooldownRemainingMs = static_cast<uint32_t>(CooldownRemaining().count());
    return stats;
  }

  RecoveryRuntimeSnapshot GetRecoveryRuntimeSnapshot() const {
    return recoveryCoordinator_.GetSnapshot(
        CurrentSessionId(), lastClosedSessionId_.load(std::memory_order_acquire));
  }

  StatusCode InvokeWithRecovery(const std::function<StatusCode()>& invoke,
                                uint32_t minRecoveryWaitMs,
                                uint32_t retryGraceMs) {
    if (!invoke) {
      HILOGE("RpcClient::InvokeWithRecovery failed: invoke is null");
      return StatusCode::InvalidArgument;
    }

    const auto computeWaitBudget =
        [minRecoveryWaitMs, retryGraceMs](const RecoveryRuntimeSnapshot& snapshot) {
          const uint32_t effectiveDelayMs =
              std::max(minRecoveryWaitMs, snapshot.cooldownRemainingMs);
          return std::chrono::milliseconds(effectiveDelayMs + retryGraceMs);
        };

    auto deadline = std::chrono::steady_clock::now() + computeWaitBudget(GetRecoveryRuntimeSnapshot());
    StatusCode status = invoke();
    while (true) {
      const auto snapshot = GetRecoveryRuntimeSnapshot();
      deadline = std::max(deadline, std::chrono::steady_clock::now() + computeWaitBudget(snapshot));
      if (!ShouldRetryRecoveryStatus(status, snapshot)) {
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        HILOGE("RpcClient::InvokeWithRecovery timed out retrying status=%{public}d lifecycle=%{public}d cooldown_ms=%{public}u",
               static_cast<int>(status), static_cast<int>(snapshot.lifecycleState),
               snapshot.cooldownRemainingMs);
        return status;
      }

      StatusCode waitStatus = StatusCode::PeerDisconnected;
      if (snapshot.lifecycleState == ClientLifecycleState::Cooldown) {
        waitStatus = WaitForRecovery(deadline);
      } else if (snapshot.recoveryPending ||
                 snapshot.lifecycleState == ClientLifecycleState::Recovering) {
        waitStatus = WaitForRecoveryRetry(deadline);
      }
      if (waitStatus != StatusCode::Ok) {
        return status;
      }
      status = invoke();
    }
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
  std::lock_guard<std::mutex> lock(impl_->recoveryPolicyMutex_);
  impl_->recoveryPolicy_ = std::move(policy);
}

void RpcClient::RequestExternalRecovery(ExternalRecoveryRequest request) {
  impl_->RequestExternalRecovery(request);
}

StatusCode RpcClient::Init() {
  if (impl_->clientClosed_.load(std::memory_order_acquire)) {
    HILOGE("RpcClient::Init failed: client already closed");
    return StatusCode::ClientClosed;
  }
  if (impl_->running_.load(std::memory_order_acquire)) {
    HILOGW("RpcClient::Init called while already running");
    return impl_->EnsureLiveSession();
  }
  impl_->lastActivityMs_.store(MonotonicNowMs64(), std::memory_order_release);
  impl_->PrepareForInitOpen();
  impl_->StartThreads();
  const StatusCode status = impl_->EnsureLiveSession();
  if (status != StatusCode::Ok) {
    HILOGE("RpcClient::Init failed: EnsureLiveSession status=%{public}d", static_cast<int>(status));
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

StatusCode RpcClient::InvokeWithRecovery(const std::function<StatusCode()>& invoke,
                                         uint32_t minRecoveryWaitMs,
                                         uint32_t retryGraceMs) {
  return impl_->InvokeWithRecovery(invoke, minRecoveryWaitMs, retryGraceMs);
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

StatusCode RpcSyncClient::InvokeWithRecovery(const std::function<StatusCode()>& invoke,
                                             uint32_t minRecoveryWaitMs,
                                             uint32_t retryGraceMs) {
  return client_.InvokeWithRecovery(invoke, minRecoveryWaitMs, retryGraceMs);
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
