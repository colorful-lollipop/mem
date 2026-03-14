#include "client/ves_client.h"

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

VesClient::VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                     VesClientOptions options)
    : remote_(remote),
      client_(),
      options_(options) {}

VesClient::~VesClient() = default;

void VesClient::RegisterProxyFactory() {
    OHOS::BrokerRegistration::GetInstance().Register(
        VES_CONTROL_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VesControlProxy>(remote, servicePath);
        });
}

MemRpc::StatusCode VesClient::Init() {
    auto bootstrap = OHOS::iface_cast<IVesControl>(remote_);
    if (bootstrap == nullptr) {
        HILOGE("iface_cast<IVesControl> failed");
        return MemRpc::StatusCode::InvalidArgument;
    }

    proxy_ = std::dynamic_pointer_cast<VesControlProxy>(bootstrap);
    if (proxy_ == nullptr) {
        HILOGE("dynamic_pointer_cast to VesControlProxy failed");
        return MemRpc::StatusCode::InvalidArgument;
    }

    bootstrapChannel_ = std::make_shared<VesControlChannelAdapter>(proxy_);
    client_.SetBootstrapChannel(bootstrapChannel_);

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

void VesClient::Shutdown() {
    client_.Shutdown();
}

bool VesClient::EngineDied() const {
    return engineDied_.load();
}

MemRpc::StatusCode VesClient::ScanFile(const std::string& path, ScanFileReply* reply) {
    ScanFileRequest request;
    request.filePath = path;
    return MemRpc::InvokeTypedSync<ScanFileRequest, ScanFileReply>(
        &client_,
        static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        request, reply);
}

}  // namespace VirusExecutorService
