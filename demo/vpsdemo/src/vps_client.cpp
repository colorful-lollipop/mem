#include "vps_client.h"

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "vps_bootstrap_interface.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

// Internal DeathRecipient that sets an atomic flag when the engine dies.
class VpsClient::DeathRecipientImpl : public OHOS::IRemoteObject::DeathRecipient {
 public:
    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>& remote) override {
        HLOGI("death callback: engine died (OHOS path)");
        died_ = true;
    }
    bool died() const { return died_.load(); }
 private:
    std::atomic<bool> died_{false};
};

VpsClient::VpsClient(const OHOS::sptr<OHOS::IRemoteObject>& remote)
    : remote_(remote),
      client_() {}

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

    client_.SetEngineDeathHandler([](const memrpc::EngineDeathReport& report) {
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
        return memrpc::RestartDecision{memrpc::RestartAction::Restart, 200};
    });

    death_recipient_ = std::make_shared<DeathRecipientImpl>();
    remote_->AddDeathRecipient(death_recipient_);
    return client_.Init();
}

void VpsClient::Shutdown() {
    client_.Shutdown();
}

bool VpsClient::EngineDied() const {
    return death_recipient_ != nullptr && death_recipient_->died();
}

memrpc::StatusCode VpsClient::InitEngine(InitReply* reply) {
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(DemoOpcode::DemoInit);
    auto future = client_.InvokeAsync(call);
    memrpc::RpcReply raw;
    auto status = future.WaitAndTake(&raw);
    if (status != memrpc::StatusCode::Ok) {
        return status;
    }
    if (reply != nullptr && !memrpc::DecodeMessage<InitReply>(raw.payload, reply)) {
        return memrpc::StatusCode::ProtocolMismatch;
    }
    return memrpc::StatusCode::Ok;
}

memrpc::StatusCode VpsClient::ScanFile(const std::string& path, ScanFileReply* reply) {
    ScanFileRequest request;
    request.file_path = path;
    return memrpc::InvokeTypedSync<ScanFileRequest, ScanFileReply>(
        &client_,
        static_cast<memrpc::Opcode>(DemoOpcode::DemoScanFile),
        request, reply);
}

memrpc::StatusCode VpsClient::UpdateFeatureLib(UpdateFeatureLibReply* reply) {
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(DemoOpcode::DemoUpdateFeatureLib);
    auto future = client_.InvokeAsync(call);
    memrpc::RpcReply raw;
    auto status = future.WaitAndTake(&raw);
    if (status != memrpc::StatusCode::Ok) {
        return status;
    }
    if (reply != nullptr && !memrpc::DecodeMessage<UpdateFeatureLibReply>(raw.payload, reply)) {
        return memrpc::StatusCode::ProtocolMismatch;
    }
    return memrpc::StatusCode::Ok;
}

}  // namespace vpsdemo
