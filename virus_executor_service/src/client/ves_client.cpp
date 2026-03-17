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
                                             uint32_t execTimeoutMs) {
    std::vector<uint8_t> payload;
    if (!MemRpc::EncodeMessage<Request>(request, &payload)) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    if (payload.size() <= MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
        return MemRpc::InvokeTypedSync<Request, Reply>(
            client,
            static_cast<MemRpc::Opcode>(opcode),
            request,
            reply,
            priority,
            execTimeoutMs);
    }

    if (control == nullptr) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    VesAnyCallRequest anyRequest;
    anyRequest.opcode = static_cast<uint16_t>(opcode);
    anyRequest.priority = static_cast<uint16_t>(priority);
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

bool ShouldRetryAfterCooldown(MemRpc::StatusCode status, bool cooldownObserved)
{
    if (status == MemRpc::StatusCode::CooldownActive) {
        return true;
    }
    return cooldownObserved && status == MemRpc::StatusCode::PeerDisconnected;
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
    if (healthSnapshotCallback_) {
        bootstrapChannel_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }

    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [delay = options_.execTimeoutRestartDelayMs](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, delay};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    policy.onEngineDeath = [this](const MemRpc::EngineDeathReport& report) {
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
        engineDied_ = true;
        return MemRpc::RecoveryDecision{
            MemRpc::RecoveryAction::Restart,
            options_.engineDeathRestartDelayMs,
        };
    };
    if (options_.idleShutdownTimeoutMs > 0) {
        policy.onIdle = [timeout = options_.idleShutdownTimeoutMs](uint64_t idleMs) {
            if (idleMs < timeout) {
                return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::CloseSession, 0};
        };
    }

    client_.SetRecoveryPolicy(std::move(policy));
    return client_.Init();
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
}

void VesClient::Shutdown() {
    client_.Shutdown();
    bootstrapChannel_.reset();
    control_.reset();
}

bool VesClient::EngineDied() const {
    return engineDied_.load();
}

MemRpc::StatusCode VesClient::ScanFile(const ScanTask& scanTask,
                                       ScanFileReply* reply,
                                       MemRpc::Priority priority,
                                       uint32_t execTimeoutMs) {
    if (reply == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    auto invoke = [&]() {
        auto control = bootstrapChannel_ != nullptr ? bootstrapChannel_->CurrentControl() : control_;
        return InvokeWithAnyCallFallback<ScanTask, ScanFileReply>(
            &client_,
            control,
            VesOpcode::ScanFile,
            scanTask,
            reply,
            priority,
            execTimeoutMs);
    };

    MemRpc::StatusCode status = invoke();
    bool cooldownObserved = status == MemRpc::StatusCode::CooldownActive;
    if (!cooldownObserved) {
        return status;
    }

    const uint32_t configuredDelayMs =
        std::max(options_.execTimeoutRestartDelayMs, options_.engineDeathRestartDelayMs);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(configuredDelayMs) + RECOVERY_RETRY_GRACE;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(RECOVERY_RETRY_POLL_INTERVAL);
        status = invoke();
        cooldownObserved = cooldownObserved || status == MemRpc::StatusCode::CooldownActive;
        if (!ShouldRetryAfterCooldown(status, cooldownObserved)) {
            return status;
        }
    }

    return status;
}

}  // namespace VirusExecutorService
