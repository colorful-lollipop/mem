#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "iservice_registry.h"
#include "registry_backend.h"
#include "vps_bootstrap_interface.h"
#include "vps_client.h"
#include "vpsdemo_protocol.h"
#include "virus_protection_service_log.h"

namespace {

const char* REGISTRY_SOCKET_ENV = "OHOS_SA_MOCK_REGISTRY_SOCKET";
constexpr int32_t LOAD_TIMEOUT_MS = 5000;

std::unique_ptr<vpsdemo::VpsClient> ConnectToEngine() {
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->GetSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID);
    if (remote == nullptr) {
        // Service not registered yet; try LoadSystemAbility.
        remote = sam->LoadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID, LOAD_TIMEOUT_MS);
    }
    if (remote == nullptr) {
        HLOGE("ConnectToEngine failed for saId=%{public}d", vpsdemo::VPS_BOOTSTRAP_SA_ID);
        return nullptr;
    }
    HLOGI("service path: %{public}s", remote->GetServicePath().c_str());

    auto client = std::make_unique<vpsdemo::VpsClient>(remote);
    if (client->Init() != memrpc::StatusCode::Ok) {
        HLOGE("VpsClient init failed");
        return nullptr;
    }
    return client;
}

std::unique_ptr<vpsdemo::VpsClient> LoadAndConnectToEngine() {
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();

    auto remote = sam->LoadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID, LOAD_TIMEOUT_MS);
    if (remote == nullptr) {
        // Retry once after a delay.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        remote = sam->LoadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID, LOAD_TIMEOUT_MS);
    }

    if (remote == nullptr) {
        HLOGE("LoadSystemAbility failed");
        return nullptr;
    }
    HLOGI("service path: %{public}s", remote->GetServicePath().c_str());

    auto client = std::make_unique<vpsdemo::VpsClient>(remote);
    if (client->Init() != memrpc::StatusCode::Ok) {
        HLOGE("VpsClient init failed");
        return nullptr;
    }
    return client;
}

}  // namespace

int main() {
    const char* registrySocket = std::getenv(REGISTRY_SOCKET_ENV);
    if (registrySocket == nullptr) {
        HLOGE("%{public}s not set", REGISTRY_SOCKET_ENV);
        return 1;
    }

    // Inject backend and proxy factory.
    auto backend = std::make_shared<vpsdemo::RegistryBackend>(registrySocket);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    vpsdemo::VpsClient::RegisterProxyFactory();

    // --- First session ---
    HLOGI("=== First session ===");
    auto client = ConnectToEngine();
    if (!client) {
        return 1;
    }

    vpsdemo::InitReply initReply;
    client->InitEngine(&initReply);
    HLOGI("Init: code=%{public}d", initReply.code);

    vpsdemo::ScanFileReply scanReply;
    client->ScanFile("/data/test_file_1.apk", &scanReply);
    HLOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threat_level);

    vpsdemo::UpdateFeatureLibReply updateReply;
    client->UpdateFeatureLib(&updateReply);
    HLOGI("UpdateFeatureLib: code=%{public}d", updateReply.code);

    client->ScanFile("/data/test_file_2.apk", &scanReply);
    HLOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threat_level);

    client->Shutdown();

    // Request engine unload (triggers death callback).
    HLOGI("=== Unload engine ===");
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    sam->UnloadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    HLOGI("engine_died: %{public}s", client->engine_died() ? "true" : "false");

    // --- Restart: load again ---
    HLOGI("=== Second session (after restart) ===");
    auto client2 = LoadAndConnectToEngine();
    if (client2) {
        vpsdemo::InitReply reply2;
        client2->InitEngine(&reply2);
        HLOGI("Init: code=%{public}d", reply2.code);

        vpsdemo::ScanFileReply scan2;
        client2->ScanFile("/data/test_file_3.apk", &scan2);
        HLOGI("ScanFile: code=%{public}d threat=%{public}d", scan2.code, scan2.threat_level);

        client2->Shutdown();
        HLOGI("second session completed");
    } else {
        HLOGI("engine not available after restart (expected in demo)");
    }

    HLOGI("=== Done ===");
    return client->engine_died() ? 0 : 1;
}
