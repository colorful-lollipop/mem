#include "client/ves_client.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "iservice_registry.h"
#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {

constexpr std::chrono::milliseconds RECOVERY_RETRY_POLL_INTERVAL{20};
constexpr std::chrono::milliseconds RECOVERY_RETRY_GRACE{100};
constexpr int CONTROL_RELOAD_TIMEOUT_MS = 5000;

template <typename Request, typename Reply>
MemRpc::StatusCode InvokeWithAnyCallFallback(MemRpc::RpcClient* client,
                                             const OHOS::sptr<IVesControl>& control,
                                             VesOpcode opcode,
                                             const Request& request,
                                             Reply* reply,
                                             MemRpc::Priority priority,
                                             uint32_t execTimeoutMs,
                                             uint32_t recoveryTimeoutMs) {
    std::vector<uint8_t> payload;
    if (!MemRpc::EncodeMessage<Request>(request, &payload)) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    if (payload.size() <= MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
        MemRpc::RpcCall call;
        call.opcode = static_cast<MemRpc::Opcode>(opcode);
        call.priority = priority;
        call.waitForRecovery = true;
        call.recoveryTimeoutMs = recoveryTimeoutMs;
        call.execTimeoutMs = execTimeoutMs;
        call.payload = std::move(payload);
        return MemRpc::WaitAndDecode<Reply>(client->InvokeAsync(std::move(call)), reply);
    }

    if (control == nullptr) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    VesAnyCallRequest anyRequest;
    anyRequest.opcode = static_cast<uint16_t>(opcode);
    anyRequest.priority = static_cast<uint16_t>(priority);
    anyRequest.timeoutMs = execTimeoutMs;
    anyRequest.payload = std::move(payload);

    VesAnyCallReply anyReply;
    const MemRpc::StatusCode status = control->AnyCall(anyRequest, anyReply);
    if (status != MemRpc::StatusCode::Ok) {
        return status;
    }
    if (anyReply.status != MemRpc::StatusCode::Ok) {
        return anyReply.status;
    }
    return MemRpc::DecodeMessage<Reply>(anyReply.payload, reply)
               ? MemRpc::StatusCode::Ok
               : MemRpc::StatusCode::ProtocolMismatch;
}

bool ShouldRetryRecoveryStatus(MemRpc::StatusCode status,
                               const MemRpc::RecoveryRuntimeSnapshot& snapshot)
{
    if (snapshot.terminalManualShutdown ||
        snapshot.lifecycleState == MemRpc::ClientLifecycleState::Closed) {
        return false;
    }
    if (status == MemRpc::StatusCode::CooldownActive) {
        return snapshot.lifecycleState == MemRpc::ClientLifecycleState::Cooldown ||
               snapshot.lifecycleState == MemRpc::ClientLifecycleState::Recovering;
    }
    if (status == MemRpc::StatusCode::PeerDisconnected) {
        return snapshot.recoveryPending ||
               snapshot.lifecycleState == MemRpc::ClientLifecycleState::Recovering ||
               snapshot.lifecycleState == MemRpc::ClientLifecycleState::Cooldown;
    }
    return false;
}

MemRpc::RecoveryPolicy BuildRecoveryPolicy(const VesClientOptions& options,
                                           const std::function<void()>& notifyRecoveryState)
{
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [notifyRecoveryState, delay = options.execTimeoutRestartDelayMs](
                           const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            if (notifyRecoveryState) {
                notifyRecoveryState();
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, delay};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    policy.onEngineDeath = [notifyRecoveryState, restartDelayMs = options.engineDeathRestartDelayMs](
                               const MemRpc::EngineDeathReport& report) {
        HILOGW("engine death: session=%{public}llu, safe_to_replay=%{public}u, poison_pills=%{public}zu",
               static_cast<unsigned long long>(report.deadSessionId),
               report.safeToReplayCount,
               report.poisonPillSuspects.size());
        for (const auto& suspect : report.poisonPillSuspects) {
            HILOGW("  poison pill: request_id=%{public}llu, opcode=%{public}u, last_state=%{public}d",
                   static_cast<unsigned long long>(suspect.requestId),
                   static_cast<unsigned>(suspect.opcode),
                   static_cast<int>(suspect.lastState));
        }
        if (notifyRecoveryState) {
            notifyRecoveryState();
        }
        return MemRpc::RecoveryDecision{
            MemRpc::RecoveryAction::Restart,
            restartDelayMs,
        };
    };
    if (options.idleShutdownTimeoutMs > 0) {
        policy.onIdle = [timeout = options.idleShutdownTimeoutMs](uint64_t idleMs) {
            if (idleMs < timeout) {
                return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::IdleClose, 0};
        };
    }
    return policy;
}

}  // namespace

VesClient::VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                     VesClientOptions options)
    : remote_(remote),
      options_(options) {}

VesClient::~VesClient() {
    Shutdown();
}

void VesClient::RegisterProxyFactory() {
    OHOS::BrokerRegistration::GetInstance().Register(
        VES_CONTROL_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VesControlProxy>(remote, servicePath);
        });
}

std::unique_ptr<VesClient> VesClient::Connect(VesClientOptions options,
                                              VesClientConnectOptions connectOptions) {
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        HILOGE("GetSystemAbilityManager failed");
        return nullptr;
    }

    OHOS::sptr<OHOS::IRemoteObject> remote;
    if (connectOptions.checkExisting) {
        remote = sam->CheckSystemAbility(VES_CONTROL_SA_ID);
    }
    if (remote == nullptr && connectOptions.loadIfMissing) {
        remote = sam->LoadSystemAbility(VES_CONTROL_SA_ID, connectOptions.loadTimeoutMs);
    }
    if (remote == nullptr) {
        HILOGE("VesClient::Connect failed for saId=%{public}d", VES_CONTROL_SA_ID);
        return nullptr;
    }

    auto client = std::make_unique<VesClient>(remote, options);
    if (client->Init() != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient init failed");
        return nullptr;
    }
    return client;
}

MemRpc::StatusCode VesClient::Init() {
    control_ = OHOS::iface_cast<IVesControl>(remote_);
    if (control_ == nullptr) {
        HILOGE("iface_cast<IVesControl> failed");
        return MemRpc::StatusCode::InvalidArgument;
    }

    const int32_t saId = (remote_ != nullptr && remote_->GetSaId() >= 0)
                             ? remote_->GetSaId()
                             : VES_CONTROL_SA_ID;
    bootstrapChannel_ = std::make_shared<VesBootstrapChannel>(
        control_,
        [saId](bool forceReload) -> OHOS::sptr<IVesControl> {
            auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
            if (sam == nullptr) {
                return nullptr;
            }
            auto remote = forceReload ? nullptr : sam->CheckSystemAbility(saId);
            if (remote == nullptr) {
                remote = sam->LoadSystemAbility(saId, CONTROL_RELOAD_TIMEOUT_MS);
            }
            if (remote == nullptr) {
                return nullptr;
            }
            return OHOS::iface_cast<IVesControl>(remote);
        });
    client_.SetBootstrapChannel(bootstrapChannel_);
    client_.SetSessionReadyCallback([this](const MemRpc::SessionReadyReport&) {
        CacheRecoverySnapshot(client_.GetRecoveryRuntimeSnapshot());
    });
    client_.SetRecoveryEventCallback([this](const MemRpc::RecoveryEventReport& report) {
        CacheRecoveryEvent(report);
    });
    if (healthSnapshotCallback_) {
        bootstrapChannel_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }
    client_.SetRecoveryPolicy(BuildRecoveryPolicy(options_, [this]() {
        CacheRecoverySnapshot(client_.GetRecoveryRuntimeSnapshot());
    }));
    const MemRpc::StatusCode status = client_.Init();
    CacheRecoverySnapshot(client_.GetRecoveryRuntimeSnapshot());
    return status;
}

void VesClient::SetEventCallback(EventCallback callback) {
    client_.SetEventCallback(std::move(callback));
}

void VesClient::SetHealthSnapshotCallback(HealthSnapshotCallback callback) {
    healthSnapshotCallback_ = std::move(callback);
    if (bootstrapChannel_ != nullptr) {
        bootstrapChannel_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }
}

void VesClient::RequestRecovery(uint32_t delayMs) {
    client_.RequestExternalRecovery({
        MemRpc::ExternalRecoverySignal::ChannelHealthTimeout,
        0,
        delayMs,
    });
    CacheRecoverySnapshot(client_.GetRecoveryRuntimeSnapshot());
}

void VesClient::Shutdown() {
    client_.Shutdown();
    CacheRecoverySnapshot(client_.GetRecoveryRuntimeSnapshot());
    bootstrapChannel_.reset();
    control_.reset();
}

bool VesClient::EngineDied() const {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    return lastObservedTrigger_ == MemRpc::RecoveryTrigger::EngineDeath;
}

MemRpc::RecoveryRuntimeSnapshot VesClient::GetRecoveryRuntimeSnapshot() const {
    return GetCachedRecoverySnapshot();
}

MemRpc::StatusCode VesClient::ScanFile(const ScanTask& scanTask,
                                       ScanFileReply* reply,
                                       MemRpc::Priority priority,
                                       uint32_t execTimeoutMs) {
    if (reply == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    return InvokeWithRecovery([&]() {
        auto control = bootstrapChannel_ != nullptr ? bootstrapChannel_->CurrentControl() : control_;
        const auto recoveryTimeoutMs =
            static_cast<uint32_t>(RecoveryWaitTimeout(GetCachedRecoverySnapshot()).count());
        return InvokeWithAnyCallFallback<ScanTask, ScanFileReply>(
            &client_,
            control,
            VesOpcode::ScanFile,
            scanTask,
            reply,
            priority,
            execTimeoutMs,
            recoveryTimeoutMs);
    });
}

MemRpc::StatusCode VesClient::InvokeWithRecovery(const std::function<MemRpc::StatusCode()>& invoke)
{
    if (!invoke) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    auto deadline =
        std::chrono::steady_clock::now() + RecoveryWaitTimeout(GetCachedRecoverySnapshot());
    MemRpc::StatusCode status = invoke();
    while (true) {
        const auto snapshot = GetCachedRecoverySnapshot();
        deadline = std::max(deadline,
            std::chrono::steady_clock::now() + RecoveryWaitTimeout(snapshot));
        if (!ShouldRetryRecoveryStatus(status, snapshot)) {
            return status;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return status;
        }
        WaitForRecoveryRetry(deadline);
        status = invoke();
    }
}

void VesClient::CacheRecoverySnapshot(const MemRpc::RecoveryRuntimeSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    recoverySnapshot_ = snapshot;
    if (snapshot.lastTrigger != MemRpc::RecoveryTrigger::Unknown &&
        snapshot.lastTrigger != MemRpc::RecoveryTrigger::DemandReconnect) {
        lastObservedTrigger_ = snapshot.lastTrigger;
    }
    ++recoveryStateVersion_;
    recoveryCv_.notify_all();
}

void VesClient::CacheRecoveryEvent(const MemRpc::RecoveryEventReport& report)
{
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    lastRecoveryEvent_ = report;
    hasRecoveryEvent_ = true;
    if (report.trigger != MemRpc::RecoveryTrigger::Unknown &&
        report.trigger != MemRpc::RecoveryTrigger::DemandReconnect) {
        lastObservedTrigger_ = report.trigger;
    }
    recoverySnapshot_ = client_.GetRecoveryRuntimeSnapshot();
    ++recoveryStateVersion_;
    recoveryCv_.notify_all();
}

bool VesClient::WaitForRecoveryRetry(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(recoveryMutex_);
    const uint64_t observedVersion = recoveryStateVersion_;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return false;
    }
    const auto preciseCooldownWait = std::chrono::milliseconds(recoverySnapshot_.cooldownRemainingMs);
    const auto waitSlice =
        preciseCooldownWait > std::chrono::milliseconds::zero()
            ? preciseCooldownWait
            : (recoverySnapshot_.lifecycleState == MemRpc::ClientLifecycleState::Cooldown ||
                       recoverySnapshot_.lifecycleState == MemRpc::ClientLifecycleState::Recovering
                   ? RECOVERY_RETRY_POLL_INTERVAL
                   : std::chrono::milliseconds::zero());
    if (waitSlice <= std::chrono::milliseconds::zero()) {
        return false;
    }
    const auto wakeTime = std::min(deadline, now + waitSlice);
    return recoveryCv_.wait_until(lock, wakeTime, [&] {
        return recoveryStateVersion_ != observedVersion;
    });
}

MemRpc::RecoveryRuntimeSnapshot VesClient::GetCachedRecoverySnapshot() const
{
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    return recoverySnapshot_;
}

std::chrono::milliseconds VesClient::RecoveryWaitTimeout(
    const MemRpc::RecoveryRuntimeSnapshot& snapshot) const
{
    const uint32_t configuredDelayMs =
        std::max(options_.execTimeoutRestartDelayMs, options_.engineDeathRestartDelayMs);
    const uint32_t effectiveDelayMs = std::max(configuredDelayMs, snapshot.cooldownRemainingMs);
    return std::chrono::milliseconds(effectiveDelayMs) + RECOVERY_RETRY_GRACE;
}

}  // namespace VirusExecutorService
