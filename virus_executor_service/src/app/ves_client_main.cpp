#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/ves_bootstrap_interface.h"
#include "client/ves_client.h"
#include "virus_protection_service_log.h"

namespace {

const char* REGISTRY_SOCKET_ENV = "OHOS_SA_MOCK_REGISTRY_SOCKET";
constexpr int32_t LOAD_TIMEOUT_MS = 5000;

std::unique_ptr<VirusExecutorService::VesClient> ConnectToEngine() {
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->GetSystemAbility(VirusExecutorService::VES_SA_ID);
    if (remote == nullptr) {
        // Service not registered yet; try LoadSystemAbility.
        remote = sam->LoadSystemAbility(VirusExecutorService::VES_SA_ID, LOAD_TIMEOUT_MS);
    }
    if (remote == nullptr) {
        HILOGE("ConnectToEngine failed for saId=%{public}d", VirusExecutorService::VES_SA_ID);
        return nullptr;
    }

    auto client = std::make_unique<VirusExecutorService::VesClient>(remote);
    if (client->Init() != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient init failed");
        return nullptr;
    }
    return client;
}

std::unique_ptr<VirusExecutorService::VesClient> LoadAndConnectToEngine() {
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();

    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_SA_ID, LOAD_TIMEOUT_MS);
    if (remote == nullptr) {
        // Retry once after a delay.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        remote = sam->LoadSystemAbility(VirusExecutorService::VES_SA_ID, LOAD_TIMEOUT_MS);
    }

    if (remote == nullptr) {
        HILOGE("LoadSystemAbility failed");
        return nullptr;
    }
    HILOGI("service path: %{public}s", remote->GetServicePath().c_str());

    auto client = std::make_unique<VirusExecutorService::VesClient>(remote);
    if (client->Init() != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient init failed");
        return nullptr;
    }
    return client;
}

}  // namespace

int main() {
    const char* registrySocket = std::getenv(REGISTRY_SOCKET_ENV);
    if (registrySocket == nullptr) {
        HILOGE("%{public}s not set", REGISTRY_SOCKET_ENV);
        return 1;
    }

    // Inject backend and proxy factory.
    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(registrySocket);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    // --- First session ---
    HILOGI("=== First session ===");
    auto client = ConnectToEngine();
    if (!client) {
        return 1;
    }

    VirusExecutorService::ScanFileReply scanReply;
    client->ScanFile("/data/test_file_1.apk", &scanReply);
    HILOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threatLevel);

    client->ScanFile("/data/test_file_2.apk", &scanReply);
    HILOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threatLevel);

    // Request engine unload (triggers death callback via RecoveryPolicy).
    HILOGI("=== Unload engine ===");
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    sam->UnloadSystemAbility(VirusExecutorService::VES_SA_ID);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    HILOGI("engine_died: %{public}s", client->EngineDied() ? "true" : "false");

    client->Shutdown();

    // --- Restart: load again ---
    HILOGI("=== Second session (after restart) ===");
    auto client2 = LoadAndConnectToEngine();
    if (client2) {
        VirusExecutorService::ScanFileReply scan2;
        client2->ScanFile("/data/test_file_3.apk", &scan2);
        HILOGI("ScanFile: code=%{public}d threat=%{public}d", scan2.code, scan2.threatLevel);

        client2->Shutdown();
        HILOGI("second session completed");
    } else {
        HILOGI("engine not available after restart (expected in demo)");
    }

    HILOGI("=== Done ===");
    return client->EngineDied() ? 0 : 1;
}
