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

bool IsHighPriority(const RpcCall& call) {
  return call.priority == Priority::High;
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
      *reply = RpcReply{StatusCode::PeerDisconnected};
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
      *reply = RpcReply{StatusCode::PeerDisconnected};
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
      *reply = RpcReply{StatusCode::PeerDisconnected};
    }
    return StatusCode::PeerDisconnected;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->cv.wait_for(lock, timeout, [this] { return state_->ready; })) {
    if (reply != nullptr) {
      *reply = RpcReply{StatusCode::QueueTimeout};
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
  RecoveryPolicy recoveryPolicy_;
  RpcEventCallback eventCallback_;
  SessionReadyCallback sessionReadyCallback_;
  std::deque<PendingSubmit> submitQueue_;
  std::unordered_map<uint64_t, PendingRequest> pending_;
  std::condition_variable submitCv_;
  std::thread submitThread_;
  std::thread responseThread_;
  std::thread watchdogThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shuttingDown{false};
  std::atomic<bool> clientClosed_{false};
  std::atomic<bool> submitterWaitingForCredit_{false};
  std::atomic<uint64_t> nextRequestId_{1};
  std::atomic<uint64_t> cooldownUntilMs_{0};
  std::atomic<uint64_t> lastActivityMs_{0};
  std::atomic<uint64_t> lastClosedSessionId_{0};
  std::atomic<uint64_t> currentSessionId_{0};
  std::atomic<uint32_t> sessionGeneration_{0};
  SessionOpenReason nextSessionOpenReason_{SessionOpenReason::InitialInit};
  uint32_t nextSessionOpenDelayMs_ = 0;

  static RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(state);
  }

  void TouchActivity() {
    lastActivityMs_.store(MonotonicNowMs64(), std::memory_order_release);
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

  void MarkNextSessionOpen(SessionOpenReason reason, uint32_t delayMs) {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    nextSessionOpenReason_ = reason;
    nextSessionOpenDelayMs_ = delayMs;
  }

  void PrepareForInitOpen() {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    nextSessionOpenReason_ = sessionGeneration_.load(std::memory_order_acquire) == 0
                                 ? SessionOpenReason::InitialInit
                                 : SessionOpenReason::DemandReconnect;
    nextSessionOpenDelayMs_ = 0;
  }

  void NotifySessionReady(uint64_t sessionId) {
    SessionReadyCallback callback;
    {
      std::lock_guard<std::mutex> lock(sessionReadyMutex_);
      callback = sessionReadyCallback_;
    }

    SessionReadyReport report;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      report.reason = nextSessionOpenReason_;
      report.scheduledDelayMs = nextSessionOpenDelayMs_;
      nextSessionOpenReason_ = SessionOpenReason::DemandReconnect;
      nextSessionOpenDelayMs_ = 0;
    }
    report.sessionId = sessionId;
    report.previousSessionId = lastClosedSessionId_.load(std::memory_order_acquire);
    report.generation = sessionGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    report.monotonicMs = MonotonicNowMs64();

    if (callback) {
      callback(report);
    }
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
    info.sessionId = currentSessionId_.load(std::memory_order_acquire);
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
    ApplyRecoveryDecision(decision, false);
  }

  void ApplyRecoveryDecision(const RecoveryDecision& decision, bool fromEngineDeath) {
    switch (decision.action) {
      case RecoveryAction::Ignore:
        return;
      case RecoveryAction::CloseSession:
        CloseLiveSession();
        return;
      case RecoveryAction::Restart:
        BeginRestart(decision.delayMs, fromEngineDeath);
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
      currentSessionId_.store(sessionId, std::memory_order_release);
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
      const uint64_t closedSessionId = currentSessionId_.load(std::memory_order_acquire);
      session_.Reset();
      currentSessionId_.store(0, std::memory_order_release);
      if (closedSessionId != 0) {
        lastClosedSessionId_.store(closedSessionId, std::memory_order_release);
      }
    }
    if (bootstrap != nullptr) {
      (void)bootstrap->CloseSession();
    }
  }

  bool CooldownActive() const {
    return MonotonicNowMs64() < cooldownUntilMs_.load(std::memory_order_acquire);
  }

  StatusCode EnsureLiveSession() {
    if (clientClosed_.load(std::memory_order_acquire)) {
      return StatusCode::ClientClosed;
    }
    if (CooldownActive()) {
      return StatusCode::CooldownActive;
    }
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      if (session_.Valid() && session_.State() == Session::SessionState::Alive) {
        return StatusCode::Ok;
      }
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
    running_.store(false, std::memory_order_release);
    submitCv_.notify_all();
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      if (session_.Handles().reqCreditEventFd >= 0) {
        (void)SignalEventFd(session_.Handles().reqCreditEventFd);
      }
      if (session_.Handles().respEventFd >= 0) {
        (void)SignalEventFd(session_.Handles().respEventFd);
      }
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
    clientClosed_.store(true, std::memory_order_release);
    shuttingDown.store(true, std::memory_order_release);
    StopThreads();
    CloseLiveSession();
    FailAllPending(StatusCode::PeerDisconnected);
    std::deque<PendingSubmit> queued;
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      queued.swap(submitQueue_);
    }
    for (auto& submit : queued) {
      PendingInfo info = MakePendingInfo(submit);
      FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission, submit.future);
    }
  }

  PollEventFdResult WaitForRequestCredit(std::chrono::steady_clock::time_point deadline) {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      fd = session_.Handles().reqCreditEventFd;
    }
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
    pending.info.sessionId = currentSessionId_.load(std::memory_order_acquire);
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

  void SubmitOne(PendingSubmit submit) {
    const bool infiniteWait = submit.call.admissionTimeoutMs == 0;
    const auto deadline = infiniteWait ? std::chrono::steady_clock::time_point::max()
                                       : std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(submit.call.admissionTimeoutMs);
    PendingInfo info = MakePendingInfo(submit);

    while (running_.load(std::memory_order_acquire)) {
      const StatusCode sessionStatus = EnsureLiveSession();
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
      SubmitOne(std::move(submit));
    }
  }

  void ResolveCompletedRequest(const ResponseRingEntry& entry) {
    PendingRequest pending;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      const auto it = pending_.find(entry.requestId);
      if (it != pending_.end()) {
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
        HandleEngineDeath(currentSessionId_.load(std::memory_order_acquire));
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
    while (running_.load(std::memory_order_acquire)) {
      if (DrainResponseRing()) {
        continue;
      }

      int fd = -1;
      {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_.Valid()) {
          fd = session_.Handles().respEventFd;
        }
      }
      if (fd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      pollfd pollFd{fd, POLLIN, 0};
      const auto waitResult = PollEventFd(&pollFd, 100);
      if (waitResult == PollEventFdResult::Failed) {
        HandleEngineDeath(currentSessionId_.load(std::memory_order_acquire));
      }
    }
  }

  void MaybeRunIdlePolicy() {
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
    const uint64_t sessionId = currentSessionId_.load(std::memory_order_acquire);
    if (sessionId == 0) {
      return;
    }
    const uint64_t nowMs = MonotonicNowMs64();
    const uint64_t idleMs = nowMs - lastActivityMs_.load(std::memory_order_acquire);
    const RecoveryDecision decision = policy.onIdle(idleMs);
    ApplyRecoveryDecision(decision, false);
  }

  void MaybeRunHealthCheck() {
    std::shared_ptr<IBootstrapChannel> bootstrap;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      bootstrap = bootstrap_;
    }
    if (bootstrap == nullptr) {
      return;
    }
    const uint64_t expectedSessionId = currentSessionId_.load(std::memory_order_acquire);
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
      MaybeRunHealthCheck();
      MaybeRunIdlePolicy();
      std::this_thread::sleep_for(std::min(kHealthCheckPeriod, kIdlePollPeriod));
    }
  }

  void BeginRestart(uint32_t delayMs, bool fromEngineDeath) {
    MarkNextSessionOpen(SessionOpenReason::RestartRecovery, delayMs);
    cooldownUntilMs_.store(MonotonicNowMs64() + delayMs, std::memory_order_release);
    CloseLiveSession();
    if (!fromEngineDeath) {
      FailAllPending(StatusCode::PeerDisconnected);
    }
    if (delayMs == 0) {
      (void)EnsureLiveSession();
    }
  }

  void HandleEngineDeath(uint64_t sessionId) {
    const uint64_t current = currentSessionId_.load(std::memory_order_acquire);
    if (sessionId != 0 && current != 0 && sessionId != current) {
      return;
    }

    CloseLiveSession();
    FailAllPending(StatusCode::CrashedDuringExecution);

    RecoveryPolicy policy;
    {
      std::lock_guard<std::mutex> lock(recoveryMutex_);
      policy = recoveryPolicy_;
    }
    if (!policy.onEngineDeath) {
      return;
    }
    EngineDeathReport report;
    report.deadSessionId = sessionId == 0 ? current : sessionId;
    const RecoveryDecision decision = policy.onEngineDeath(report);
    ApplyRecoveryDecision(decision, true);
  }

  void RequestExternalRecovery(ExternalRecoveryRequest request) {
    const uint64_t current = currentSessionId_.load(std::memory_order_acquire);
    if (request.sessionId != 0 && current != 0 && request.sessionId != current) {
      return;
    }
    MarkNextSessionOpen(SessionOpenReason::ExternalRecovery, request.delayMs);
    cooldownUntilMs_.store(MonotonicNowMs64() + request.delayMs, std::memory_order_release);
    CloseLiveSession();
    if (request.delayMs == 0) {
      (void)EnsureLiveSession();
    }
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
      info.sessionId = currentSessionId_.load(std::memory_order_acquire);
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
    return stats;
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

void RpcClient::SetRecoveryPolicy(RecoveryPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->recoveryMutex_);
  impl_->recoveryPolicy_ = std::move(policy);
}

void RpcClient::RequestExternalRecovery(ExternalRecoveryRequest request) {
  impl_->RequestExternalRecovery(request);
}

StatusCode RpcClient::Init() {
  if (impl_->running_.load(std::memory_order_acquire)) {
    return impl_->EnsureLiveSession();
  }
  impl_->clientClosed_.store(false, std::memory_order_release);
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

void RpcSyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace MemRpc
