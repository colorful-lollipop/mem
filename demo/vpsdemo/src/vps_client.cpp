#include "vps_client.h"

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "vps_bootstrap_interface.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

VpsClient::VpsClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                     VpsClientOptions options)
    : remote_(remote),
      client_(),
      options_(options) {}

VpsClient::~VpsClient() = default;

void VpsClient::RegisterProxyFactory() {
    OHOS::BrokerRegistration::GetInstance().Register(
        VPS_BOOTSTRAP_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VpsBootstrapProxy>(remote, servicePath);
        });
}

memrpc::StatusCode VpsClient::Init() {
    // Use iface_cast to get the proxy (BrokerRegistration creates it for cross-process).
    auto bootstrap = OHOS::iface_cast<IVpsBootstrap>(remote_);
    if (bootstrap == nullptr) {
        HLOGE("iface_cast<IVpsBootstrap> failed");
        return memrpc::StatusCode::InvalidArgument;
    }

    // Alias shared_ptr to use VpsBootstrapProxy as IBootstrapChannel.
    proxy_ = std::dynamic_pointer_cast<VpsBootstrapProxy>(bootstrap);
    if (proxy_ == nullptr) {
        HLOGE("dynamic_pointer_cast to VpsBootstrapProxy failed");
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
        HLOGW("engine death: session=%{public}llu, safe_to_replay=%{public}u, poison_pills=%{public}zu",
              static_cast<unsigned long long>(report.dead_session_id),
              report.safe_to_replay_count,
              report.poison_pill_suspects.size());
        for (const auto& suspect : report.poison_pill_suspects) {
            HLOGW("  poison pill: request_id=%{public}llu, opcode=%{public}u, last_state=%{public}d",
                  static_cast<unsigned long long>(suspect.request_id),
                  static_cast<unsigned>(suspect.opcode),
                  static_cast<int>(suspect.last_state));
        }
        engine_died_ = true;
        if (restart_callback_) {
            restart_callback_();
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

void VpsClient::Shutdown() {
    client_.Shutdown();
}

void VpsClient::SetEngineRestartCallback(EngineRestartCallback callback) {
    restart_callback_ = std::move(callback);
}

bool VpsClient::EngineDied() const {
    return engine_died_.load();
}

memrpc::StatusCode VpsClient::ScanFile(const std::string& path, ScanFileReply* reply) {
    ScanFileRequest request;
    request.file_path = path;
    return memrpc::InvokeTypedSync<ScanFileRequest, ScanFileReply>(
        &client_,
        static_cast<memrpc::Opcode>(DemoOpcode::DemoScanFile),
        request, reply);
}

}  // namespace vpsdemo
