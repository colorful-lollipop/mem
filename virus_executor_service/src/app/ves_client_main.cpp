#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/ves_control_interface.h"
#include "client/ves_client.h"
#include "virus_protection_service_log.h"

namespace {

const char* REGISTRY_SOCKET_ENV = "OHOS_SA_MOCK_REGISTRY_SOCKET";
constexpr int32_t LOAD_TIMEOUT_MS = 5000;

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
    bool secondSessionSucceeded = false;

    // --- First session ---
    HILOGI("=== First session ===");
    auto client = VirusExecutorService::VesClient::Connect();
    if (!client) {
        return 1;
    }

    VirusExecutorService::ScanFileReply scanReply;
    VirusExecutorService::ScanTask firstTask{"/data/test_file_1.apk"};
    client->ScanFile(firstTask, &scanReply);
    HILOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threatLevel);

    VirusExecutorService::ScanTask secondTask{"/data/test_file_2.apk"};
    client->ScanFile(secondTask, &scanReply);
    HILOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threatLevel);

    // Request engine unload (triggers death callback via RecoveryPolicy).
    HILOGI("=== Unload engine ===");
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    sam->UnloadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID);

    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto firstSnapshot = client->GetRecoveryRuntimeSnapshot();
    HILOGI("last_trigger=%{public}d lifecycle=%{public}d",
           static_cast<int>(firstSnapshot.lastTrigger),
           static_cast<int>(firstSnapshot.lifecycleState));

    client->Shutdown();

    // --- Restart: load again ---
    HILOGI("=== Second session (after restart) ===");
    VirusExecutorService::VesClientConnectOptions connectOptions;
    connectOptions.loadTimeoutMs = LOAD_TIMEOUT_MS;

    auto client2 = VirusExecutorService::VesClient::Connect({}, connectOptions);
    if (!client2) {
        // Retry once after a delay.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        client2 = VirusExecutorService::VesClient::Connect({}, connectOptions);
    }
    if (client2) {
        VirusExecutorService::ScanFileReply scan2;
        VirusExecutorService::ScanTask thirdTask{"/data/test_file_3.apk"};
        client2->ScanFile(thirdTask, &scan2);
        HILOGI("ScanFile: code=%{public}d threat=%{public}d", scan2.code, scan2.threatLevel);

        client2->Shutdown();
        secondSessionSucceeded = true;
        HILOGI("second session completed");
    } else {
        HILOGI("engine not available after restart (expected in demo)");
    }

    HILOGI("=== Done ===");
    const auto finalSnapshot = client->GetRecoveryRuntimeSnapshot();
    HILOGI("last_trigger=%{public}d lifecycle=%{public}d second_session_succeeded=%{public}s",
           static_cast<int>(finalSnapshot.lastTrigger),
           static_cast<int>(finalSnapshot.lifecycleState),
           secondSessionSucceeded ? "true" : "false");
    return secondSessionSucceeded ? 0 : 1;
}
