#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "registry_client.h"
#include "vps_client.h"
#include "vpsdemo_protocol.h"
#include "virus_protection_service_log.h"

namespace {

const char* kRegistrySocketEnv = "OHOS_SA_MOCK_REGISTRY_SOCKET";

std::unique_ptr<vpsdemo::VpsClient> ConnectToEngine(
    const std::string& registrySocket) {
    vpsdemo::RegistryClient registry(registrySocket);
    std::string servicePath = registry.GetServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    }
    if (servicePath.empty()) {
        HLOGE("failed to get service path from registry");
        return nullptr;
    }
    HLOGI("service path: %{public}s", servicePath.c_str());

    auto client = std::make_unique<vpsdemo::VpsClient>(servicePath);
    if (client->Init() != memrpc::StatusCode::Ok) {
        HLOGE("VpsClient init failed");
        return nullptr;
    }
    return client;
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
    auto client = ConnectToEngine(registrySocket);
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
    vpsdemo::RegistryClient registry(registrySocket);
    registry.UnloadService(vpsdemo::kVpsBootstrapSaId);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    HLOGI("engine_died: %{public}s", client->engine_died() ? "true" : "false");

    // --- Restart: load again ---
    HLOGI("=== Second session (after restart) ===");
    std::string servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    if (servicePath.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        servicePath = registry.LoadServicePath(vpsdemo::kVpsBootstrapSaId);
    }

    if (!servicePath.empty()) {
        auto client2 = ConnectToEngine(registrySocket);
        if (client2) {
            vpsdemo::InitReply reply2;
            client2->InitEngine(&reply2);
            HLOGI("Init: code=%{public}d", reply2.code);

            vpsdemo::ScanFileReply scan2;
            client2->ScanFile("/data/test_file_3.apk", &scan2);
            HLOGI("ScanFile: code=%{public}d threat=%{public}d", scan2.code, scan2.threat_level);

            client2->Shutdown();
            HLOGI("second session completed");
        }
    } else {
        HLOGI("engine not available after restart (expected in demo)");
    }

    HLOGI("=== Done ===");
    return client->engine_died() ? 0 : 1;
}
