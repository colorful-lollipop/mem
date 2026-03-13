#include "vps_client.h"

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "vps_bootstrap_interface.h"
#include "ves_codec.h"
#include "ves_protocol.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

VesClient::VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                     VesClientOptions options)
    : remote_(remote),
      client_(),
      options_(options) {}

VesClient::~VesClient() = default;

void VesClient::RegisterProxyFactory() {
    OHOS::BrokerRegistration::GetInstance().Register(
        VPS_BOOTSTRAP_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VesBootstrapProxy>(remote, servicePath);
        });
}

memrpc::StatusCode VesClient::Init() {
    // Use iface_cast to get the proxy (BrokerRegistration creates it for cross-process).
    auto bootstrap = OHOS::iface_cast<IVesBootstrap>(remote_);
    if (bootstrap == nullptr) {
        HILOGE("iface_cast<IVesBootstrap> failed");
        return memrpc::StatusCode::InvalidArgument;
    }

    // Alias shared_ptr to use VesBootstrapProxy as IBootstrapChannel.
    proxy_ = std::dynamic_pointer_cast<VesBootstrapProxy>(bootstrap);
    if (proxy_ == nullptr) {
        HILOGE("dynamic_pointer_cast to VesBootstrapProxy failed");
        return memrpc::StatusCode::InvalidArgument;
    }

    // Set the bootstrap channel on the RpcClient.
    client_.SetBootstrapChannel(std::static_pointer_cast<memrpc::IBootstrapChannel>(proxy_));

    memrpc::RecoveryPolicy policy;
    policy.onFailure = [delay = options_.execTimeoutRestartDelayMs](const memrpc::RpcFailure& failure) {
        if (failure.status == memrpc::StatusCode::ExecTimeout) {
            return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, delay};
        }
        return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
    };
    policy.onEngineDeath = [this](const memrpc::EngineDeathReport& report) {
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
        if (restartCallback_) {
            restartCallback_();
        }
        return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, options_.engineDeathRestartDelayMs};
    };
    if (options_.idleRestartDelayMs > 0) {
        policy.onIdle = [delay = options_.idleRestartDelayMs](uint64_t) {
            return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, delay};
        };
    }
    client_.SetRecoveryPolicy(std::move(policy));

    return client_.Init();
}

void VesClient::Shutdown() {
    client_.Shutdown();
}

void VesClient::SetEngineRestartCallback(EngineRestartCallback callback) {
    restartCallback_ = std::move(callback);
}

bool VesClient::EngineDied() const {
    return engineDied_.load();
}

memrpc::StatusCode VesClient::ScanFile(const std::string& path, ScanFileReply* reply) {
    ScanFileRequest request;
    request.filePath = path;
    return memrpc::InvokeTypedSync<ScanFileRequest, ScanFileReply>(
        &client_,
        static_cast<memrpc::Opcode>(VesOpcode::ScanFile),
        request, reply);
}

}  // namespace vpsdemo
