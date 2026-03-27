#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "client/ves_client.h"
#include "client/internal/ves_client_recovery_access.h"
#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/ves_control_interface.h"
#include "virus_protection_executor_log.h"

namespace {

const char* REGISTRY_SOCKET_ENV = "OHOS_SA_MOCK_REGISTRY_SOCKET";

std::shared_ptr<VirusExecutorService::RegistryBackend> CreateRegistryBackend()
{
    const char* registrySocket = std::getenv(REGISTRY_SOCKET_ENV);
    if (registrySocket == nullptr) {
        HILOGE("%{public}s not set", REGISTRY_SOCKET_ENV);
        return nullptr;
    }
    return std::make_shared<VirusExecutorService::RegistryBackend>(registrySocket);
}

void RunScan(VirusExecutorService::VesClient& client, const char* path)
{
    VirusExecutorService::ScanFileReply scanReply;
    VirusExecutorService::ScanTask task{path};
    client.ScanFile(task, &scanReply);
    HILOGI("ScanFile: code=%{public}d threat=%{public}d", scanReply.code, scanReply.threatLevel);
}

void RunFirstSession(VirusExecutorService::VesClient& client)
{
    HILOGI("=== First session ===");
    RunScan(client, "/data/test_file_1.apk");
    RunScan(client, "/data/test_file_2.apk");
}

void UnloadEngineAndLogSnapshot(VirusExecutorService::VesClient& client)
{
    HILOGI("=== Unload engine ===");
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    sam->UnloadSystemAbility(VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID);

    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto snapshot = VirusExecutorService::internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(client);
    HILOGI("lifecycle=%{public}d", static_cast<int>(snapshot.lifecycleState));
}

bool RunSecondSession()
{
    HILOGI("=== Second session (after restart) ===");
    auto client = VirusExecutorService::VesClient::Connect();
    if (!client) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        client = VirusExecutorService::VesClient::Connect();
    }
    if (!client) {
        HILOGI("engine not available after restart (expected in demo)");
        return false;
    }

    RunScan(*client, "/data/test_file_3.apk");
    client->Shutdown();
    HILOGI("second session completed");
    return true;
}

int RunClientDemo()
{
    auto client = VirusExecutorService::VesClient::Connect();
    if (!client) {
        return 1;
    }

    RunFirstSession(*client);
    UnloadEngineAndLogSnapshot(*client);
    client->Shutdown();

    const bool secondSessionSucceeded = RunSecondSession();
    HILOGI("=== Done ===");
    const auto finalSnapshot = VirusExecutorService::internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(
        *client);
    HILOGI("lifecycle=%{public}d second_session_succeeded=%{public}s",
           static_cast<int>(finalSnapshot.lifecycleState),
           secondSessionSucceeded ? "true" : "false");
    return secondSessionSucceeded ? 0 : 1;
}

}  // namespace

int main()
{
    auto backend = CreateRegistryBackend();
    if (backend == nullptr) {
        return 1;
    }
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    return RunClientDemo();
}
