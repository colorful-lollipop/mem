#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/core/bootstrap.h"
#include "registry_client.h"
#include "vps_bootstrap_interface.h"
#include "vps_bootstrap_proxy.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "vpsdemo_types.h"
#include "virus_protection_service_log.h"

namespace {

const char* kRegistrySocketEnv = "OHOS_SA_MOCK_REGISTRY_SOCKET";

class VpsDeathRecipient : public OHOS::IRemoteObject::DeathRecipient {
 public:
    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>& remote) override {
        HLOGI("death callback: engine died");
        died_ = true;
    }
    bool died() const { return died_; }
 private:
    bool died_ = false;
};

bool DoInit(memrpc::RpcClient& client) {
    // Init takes no request payload — send empty and decode InitReply.
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(vpsdemo::DemoOpcode::DemoInit);
    auto future = client.InvokeAsync(call);
    memrpc::RpcReply reply;
    auto status = future.WaitAndTake(&reply);
    if (status != memrpc::StatusCode::Ok) {
        HLOGE("Init RPC failed: %{public}d", static_cast<int>(status));
        return false;
    }
    vpsdemo::InitReply result;
    if (!memrpc::DecodeMessage<vpsdemo::InitReply>(reply.payload, &result)) {
        HLOGE("Init: decode failed");
        return false;
    }
    HLOGI("Init: code=%{public}d", result.code);
    return true;
}

bool DoScanFile(memrpc::RpcClient& client, const std::string& path) {
    vpsdemo::ScanFileRequest request;
    request.file_path = path;
    vpsdemo::ScanFileReply result;
    auto status = memrpc::InvokeTypedSync<vpsdemo::ScanFileRequest, vpsdemo::ScanFileReply>(
        &client,
        static_cast<memrpc::Opcode>(vpsdemo::DemoOpcode::DemoScanFile),
        request, &result);
    if (status != memrpc::StatusCode::Ok) {
        HLOGE("ScanFile RPC failed: %{public}d", static_cast<int>(status));
        return false;
    }
    HLOGI("ScanFile(%{public}s): code=%{public}d threat=%{public}d",
          path.c_str(), result.code, result.threat_level);
    return true;
}

bool DoUpdateFeatureLib(memrpc::RpcClient& client) {
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(vpsdemo::DemoOpcode::DemoUpdateFeatureLib);
    auto future = client.InvokeAsync(call);
    memrpc::RpcReply reply;
    auto status = future.WaitAndTake(&reply);
    if (status != memrpc::StatusCode::Ok) {
        HLOGE("UpdateFeatureLib RPC failed: %{public}d", static_cast<int>(status));
        return false;
    }
    vpsdemo::UpdateFeatureLibReply result;
    if (!memrpc::DecodeMessage<vpsdemo::UpdateFeatureLibReply>(reply.payload, &result)) {
        HLOGE("UpdateFeatureLib: decode failed");
        return false;
    }
    HLOGI("UpdateFeatureLib: code=%{public}d", result.code);
    return true;
}

struct VpsSession {
    std::shared_ptr<vpsdemo::VpsBootstrapProxy> proxy;
    std::unique_ptr<memrpc::RpcClient> client;
    OHOS::sptr<OHOS::IRemoteObject> remote;
};

VpsSession ConnectToEngine(const std::string& registrySocket) {
    VpsSession session;
    vpsdemo::RegistryClient registry(registrySocket);
    std::string servicePath = registry.GetServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    }
    if (servicePath.empty()) {
        HLOGE("failed to get service path from registry");
        return session;
    }
    HLOGI("service path: %{public}s", servicePath.c_str());

    auto remoteObj = std::make_shared<OHOS::IRemoteObject>();
    auto proxy = std::make_shared<vpsdemo::VpsBootstrapProxy>(remoteObj, servicePath);
    remoteObj->AttachBroker(std::dynamic_pointer_cast<OHOS::IRemoteBroker>(proxy));

    memrpc::BootstrapHandles handles;
    auto status = proxy->OpenSession(&handles);
    if (status != memrpc::StatusCode::Ok) {
        HLOGE("OpenSession failed: %{public}d", static_cast<int>(status));
        return session;
    }
    HLOGI("OpenSession ok, session_id=%{public}lu",
          static_cast<unsigned long>(handles.session_id));

    class HandleBootstrap : public memrpc::IBootstrapChannel {
     public:
        explicit HandleBootstrap(const memrpc::BootstrapHandles& h) : handles_(h) {}
        memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* out) override {
            if (out == nullptr) return memrpc::StatusCode::InvalidArgument;
            *out = handles_;
            return memrpc::StatusCode::Ok;
        }
        memrpc::StatusCode CloseSession() override { return memrpc::StatusCode::Ok; }
        void SetEngineDeathCallback(memrpc::EngineDeathCallback) override {}
     private:
        memrpc::BootstrapHandles handles_;
    };

    auto bootstrap = std::make_shared<HandleBootstrap>(handles);
    auto rpcClient = std::make_unique<memrpc::RpcClient>(bootstrap);
    if (rpcClient->Init() != memrpc::StatusCode::Ok) {
        HLOGE("RpcClient init failed");
        return session;
    }

    session.proxy = proxy;
    session.client = std::move(rpcClient);
    session.remote = remoteObj;
    return session;
}

}  // namespace

int main() {
    const char* registrySocket = std::getenv(kRegistrySocketEnv);
    if (registrySocket == nullptr) {
        HLOGE("%{public}s not set", kRegistrySocketEnv);
        return 1;
    }

    // --- First session ---
    HLOGI("=== First session ===");
    auto session = ConnectToEngine(registrySocket);
    if (!session.client) {
        return 1;
    }

    auto deathRecipient = std::make_shared<VpsDeathRecipient>();
    session.remote->AddDeathRecipient(deathRecipient);

    DoInit(*session.client);
    DoScanFile(*session.client, "/data/test_file_1.apk");
    DoUpdateFeatureLib(*session.client);
    DoScanFile(*session.client, "/data/test_file_2.apk");

    session.client->Shutdown();
    session.proxy->CloseSession();

    // Request engine unload (triggers death callback).
    HLOGI("=== Unload engine ===");
    vpsdemo::RegistryClient registry(registrySocket);
    registry.UnloadService(vpsdemo::kVpsBootstrapSaId);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    HLOGI("death_recipient_called: %{public}s",
          deathRecipient->died() ? "true" : "false");

    // --- Restart: load again ---
    HLOGI("=== Second session (after restart) ===");
    std::string servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    }

    if (!servicePath.empty()) {
        auto session2 = ConnectToEngine(registrySocket);
        if (session2.client) {
            DoInit(*session2.client);
            DoScanFile(*session2.client, "/data/test_file_3.apk");
            session2.client->Shutdown();
            session2.proxy->CloseSession();
            HLOGI("second session completed");
        }
    } else {
        HLOGI("engine not available after restart (expected in demo)");
    }

    HLOGI("=== Done ===");
    return deathRecipient->died() ? 0 : 1;
}
