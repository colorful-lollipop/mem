#include "vps_client.h"

#include "iremote_broker.h"
#include "memrpc/client/typed_invoker.h"
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

VpsClient::VpsClient(const std::string& servicePath)
    : remote_(std::make_shared<OHOS::IRemoteObject>()),
      proxy_(std::make_shared<VpsBootstrapProxy>(remote_, servicePath)),
      client_(std::shared_ptr<memrpc::IBootstrapChannel>(
          proxy_, static_cast<memrpc::IBootstrapChannel*>(proxy_.get()))) {
    remote_->AttachBroker(
        std::dynamic_pointer_cast<OHOS::IRemoteBroker>(proxy_));
}

VpsClient::~VpsClient() = default;

memrpc::StatusCode VpsClient::Init() {
    death_recipient_ = std::make_shared<DeathRecipientImpl>();
    remote_->AddDeathRecipient(death_recipient_);
    return client_.Init();
}

void VpsClient::Shutdown() {
    client_.Shutdown();
}

bool VpsClient::engine_died() const {
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
