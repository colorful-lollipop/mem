#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "apps/vps/common/virus_protection_service_define.h"
#include "apps/vps/common/vps_codec.h"
#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "registry_client.h"
#include "vps_bootstrap_interface.h"
#include "vps_bootstrap_proxy.h"

using namespace OHOS::Security::VirusProtectionService;

namespace {

const char* kRegistrySocketEnv = "OHOS_SA_MOCK_REGISTRY_SOCKET";

memrpc::RpcCall MakeCall(memrpc::Opcode opcode, std::vector<uint8_t> payload = {}) {
    memrpc::RpcCall call;
    call.opcode = opcode;
    call.payload = std::move(payload);
    return call;
}

class VpsDeathRecipient : public OHOS::IRemoteObject::DeathRecipient {
 public:
    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>& remote) override {
        std::cout << "[client] death callback: engine died" << std::endl;
        died_ = true;
    }
    bool died() const { return died_; }
 private:
    bool died_ = false;
};

bool DoScanFile(memrpc::RpcSyncClient& client, const std::string& path) {
    ScanTask task;
    task.bundleName = "com.example.test";
    BasicFileInfo fi;
    fi.filePath = path;
    fi.fileSize = 1024;
    task.fileInfos.push_back(std::make_shared<BasicFileInfo>(fi));
    task.scanTaskType = ScanTaskType::QUICK;

    std::vector<uint8_t> reqBytes;
    if (!EncodeScanTask(task, &reqBytes)) {
        std::cerr << "[client] encode scan task failed" << std::endl;
        return false;
    }

    memrpc::RpcReply reply;
    auto status = client.InvokeSync(MakeCall(memrpc::Opcode::VpsScanFile, std::move(reqBytes)),
                                     &reply);
    if (status != memrpc::StatusCode::Ok) {
        std::cerr << "[client] ScanFile RPC failed: " << static_cast<int>(status)
                  << std::endl;
        return false;
    }

    int32_t resultCode = -1;
    ScanResult scanResult;
    if (!DecodeScanFileReply(reply.payload, &resultCode, &scanResult)) {
        std::cerr << "[client] decode scan result failed" << std::endl;
        return false;
    }

    std::cout << "[client] ScanFile(" << path << "): code=" << resultCode
              << " threat=" << static_cast<int>(scanResult.threatLevel) << std::endl;
    return true;
}

bool DoInit(memrpc::RpcSyncClient& client) {
    memrpc::RpcReply reply;
    auto status = client.InvokeSync(MakeCall(memrpc::Opcode::VpsInit), &reply);
    if (status != memrpc::StatusCode::Ok) {
        std::cerr << "[client] Init RPC failed" << std::endl;
        return false;
    }
    int32_t code = -1;
    DecodeInt32Result(reply.payload, &code);
    std::cout << "[client] Init: code=" << code << std::endl;
    return true;
}

bool DoUpdateFeatureLib(memrpc::RpcSyncClient& client) {
    memrpc::RpcReply reply;
    auto status = client.InvokeSync(MakeCall(memrpc::Opcode::VpsUpdateFeatureLib), &reply);
    if (status != memrpc::StatusCode::Ok) {
        std::cerr << "[client] UpdateFeatureLib RPC failed" << std::endl;
        return false;
    }
    int32_t code = -1;
    DecodeInt32Result(reply.payload, &code);
    std::cout << "[client] UpdateFeatureLib: code=" << code << std::endl;
    return true;
}

struct VpsSession {
    std::shared_ptr<vpsdemo::VpsBootstrapProxy> proxy;
    std::unique_ptr<memrpc::RpcSyncClient> client;
    OHOS::sptr<OHOS::IRemoteObject> remote;
};

VpsSession ConnectToEngine(const std::string& registrySocket) {
    VpsSession session;
    vpsdemo::RegistryClient registry(registrySocket);
    std::string servicePath = registry.GetServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        // Try Load.
        servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    }
    if (servicePath.empty()) {
        std::cerr << "[client] failed to get service path from registry" << std::endl;
        return session;
    }
    std::cout << "[client] service path: " << servicePath << std::endl;

    // Create IRemoteObject + proxy.
    auto remoteObj = std::make_shared<OHOS::IRemoteObject>();
    auto proxy = std::make_shared<vpsdemo::VpsBootstrapProxy>(remoteObj, servicePath);
    remoteObj->AttachBroker(std::dynamic_pointer_cast<OHOS::IRemoteBroker>(proxy));

    // OpenSession to get BootstrapHandles via SCM_RIGHTS.
    memrpc::BootstrapHandles handles;
    auto status = proxy->OpenSession(&handles);
    if (status != memrpc::StatusCode::Ok) {
        std::cerr << "[client] OpenSession failed: " << static_cast<int>(status) << std::endl;
        return session;
    }
    std::cout << "[client] OpenSession ok, session_id=" << handles.session_id << std::endl;

    // Create an RpcSyncClient that wraps a bootstrap channel from the received handles.
    // We need an IBootstrapChannel. Create a simple one from handles.
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
    auto rpcClient = std::make_unique<memrpc::RpcSyncClient>(bootstrap);
    if (rpcClient->Init() != memrpc::StatusCode::Ok) {
        std::cerr << "[client] RpcSyncClient init failed" << std::endl;
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
        std::cerr << "[client] " << kRegistrySocketEnv << " not set" << std::endl;
        return 1;
    }

    // --- First session ---
    std::cout << "\n=== First session ===" << std::endl;
    auto session = ConnectToEngine(registrySocket);
    if (!session.client) {
        return 1;
    }

    // Install death recipient.
    auto deathRecipient = std::make_shared<VpsDeathRecipient>();
    session.remote->AddDeathRecipient(deathRecipient);

    DoInit(*session.client);
    DoScanFile(*session.client, "/data/test_file_1.apk");
    DoUpdateFeatureLib(*session.client);
    DoScanFile(*session.client, "/data/test_file_2.apk");

    // Shut down first session.
    session.client->Shutdown();
    session.proxy->CloseSession();

    // Request engine unload (triggers death callback).
    std::cout << "\n=== Unload engine ===" << std::endl;
    vpsdemo::RegistryClient registry(registrySocket);
    registry.UnloadService(vpsdemo::kVpsBootstrapSaId);

    // Give time for death notification.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "[client] death_recipient_called: "
              << (deathRecipient->died() ? "true" : "false") << std::endl;

    // --- Restart: load again ---
    std::cout << "\n=== Second session (after restart) ===" << std::endl;
    std::string servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        // Engine not restarted by supervisor in time — give more time.
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
            std::cout << "[client] second session completed" << std::endl;
        }
    } else {
        std::cout << "[client] engine not available after restart (expected in demo)"
                  << std::endl;
    }

    std::cout << "\n=== Done ===" << std::endl;
    return deathRecipient->died() ? 0 : 1;
}
