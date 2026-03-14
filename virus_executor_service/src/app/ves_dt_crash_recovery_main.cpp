// DT: crash recovery
//
// Step 1: normal scan → verify ok
// Step 2: send crash sample → engine dies
// Step 3: verify death callback fired
// Step 4: engine respawned (via DeathRecipient harness) → framework reconnects
// Step 5: normal scan again → verify ok
//
// Uses the supervisor pattern: self-host registry, fork engine, VesClient.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "iremote_object.h"
#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/registry_server.h"
#include "transport/ves_control_interface.h"
#include "client/ves_client.h"
#include "ves/ves_types.h"
#include "virus_protection_service_log.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_dt_crash_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_dt_crash_service.sock";

std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(),
              nullptr);
        HILOGE("exec engine failed");
        _exit(1);
    }
    return pid;
}

void KillAndWait(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

#define DT_CHECK(expr, msg)                              \
    do {                                                 \
        if (!(expr)) {                                   \
            HILOGE("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); \
            return 1;                                    \
        }                                                \
        HILOGI("PASS: %s", msg);                          \
    } while (0)

class EngineRespawnRecipient : public OHOS::IRemoteObject::DeathRecipient {
 public:
    EngineRespawnRecipient(const std::string& enginePath, std::atomic<int>* engineRestarts)
        : enginePath_(enginePath), engineRestarts_(engineRestarts) {}

    void Disable()
    {
        enabled_.store(false);
    }

    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>&) override
    {
        if (!enabled_.load()) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            int status = 0;
            waitpid(g_engine_pid, &status, WNOHANG);
            g_engine_pid = -1;
        }
        g_engine_pid = SpawnEngine(enginePath_);
        if (g_engine_pid > 0) {
            engineRestarts_->fetch_add(1);
            HILOGI("engine respawned pid=%{public}d", g_engine_pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

 private:
    std::string enginePath_;
    std::atomic<int>* engineRestarts_;
    std::atomic<bool> enabled_{true};
};

}  // namespace

int main(int argc, char* argv[]) {
    // Determine engine path relative to our binary.
    std::string dir = ".";
    std::string argv0(argv[0]);
    auto pos = argv0.rfind('/');
    if (pos != std::string::npos) {
        dir = argv0.substr(0, pos);
    }
    std::string enginePath = dir + "/VirusExecutorService";

    // --- Setup: registry + engine + client ---
    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);

    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) return true;
        HILOGI("load callback: spawning engine");
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    });

    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    DT_CHECK(registry.Start(), "registry started");

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    DT_CHECK(g_engine_pid > 0, "engine spawned");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
    DT_CHECK(remote != nullptr, "LoadSystemAbility ok");

    std::atomic<int> engineRestarts{0};
    auto respawnRecipient = std::make_shared<EngineRespawnRecipient>(enginePath, &engineRestarts);
    DT_CHECK(remote->AddDeathRecipient(respawnRecipient), "DeathRecipient registered");

    auto client = std::make_unique<VirusExecutorService::VesClient>(remote);

    auto initStatus = client->Init();
    DT_CHECK(initStatus == MemRpc::StatusCode::Ok, "VesClient Init ok");

    // === Step 1: normal scan ===
    HILOGI("=== Step 1: normal scan ===");
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile("/data/dt_clean.apk", &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step1: clean scan RPC ok");
        DT_CHECK(reply.threatLevel == 0, "step1: clean file threat=0");
    }
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile("/data/dt_virus.apk", &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step1: virus scan RPC ok");
        DT_CHECK(reply.threatLevel == 1, "step1: virus file threat=1");
    }

    // === Step 2: send crash sample ===
    HILOGI("=== Step 2: send crash sample (engine will abort) ===");
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile("/data/dt_crash.apk", &reply);
        // The engine aborts — this request will fail.
        HILOGI("step2: crash scan returned status=%{public}d (expected failure)",
              static_cast<int>(status));
    }

    // === Step 3: wait for death detection + restart ===
    HILOGI("=== Step 3: waiting for engine restart ===");
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (engineRestarts.load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        DT_CHECK(engineRestarts.load() > 0, "step3: engine death detected and respawned");
    }

    // Give the framework time to re-establish the session.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // === Step 4: scan after recovery ===
    HILOGI("=== Step 4: scan after recovery ===");
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile("/data/dt_clean_after.apk", &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step4: post-recovery clean scan ok");
        DT_CHECK(reply.threatLevel == 0, "step4: post-recovery threat=0");
    }
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile("/data/dt_virus_after.apk", &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step4: post-recovery virus scan ok");
        DT_CHECK(reply.threatLevel == 1, "step4: post-recovery threat=1");
    }

    // === Step 5: second crash + 10 sequential calls (卡住定位) ===
    HILOGI("=== Step 5: second crash + 10 sequential normal calls ===");
    int prev = engineRestarts.load();
    {
        VirusExecutorService::ScanFileReply reply;
        HILOGI("step5: sending crash...");
        auto status = client->ScanFile("/data/dt_crash2.apk", &reply);
        HILOGI("step5: crash returned status=%{public}d", static_cast<int>(status));
    }
    // Wait for restart to complete.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (engineRestarts.load() == prev &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        DT_CHECK(engineRestarts.load() > prev, "step5: second restart detected");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    for (int i = 0; i < 10; i++) {
        HILOGI("step5: call %{public}d/10 ...", i + 1);
        VirusExecutorService::ScanFileReply reply;
        const auto t0 = std::chrono::steady_clock::now();
        auto status = client->ScanFile(
            "/data/dt_post_recover_" + std::to_string(i) + ".apk", &reply);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        HILOGI("step5: call %{public}d/10 status=%{public}d threat=%{public}d elapsed=%{public}lld ms",
              i + 1, static_cast<int>(status), reply.threatLevel,
              static_cast<long long>(elapsed));
        DT_CHECK(status == MemRpc::StatusCode::Ok,
                 ("step5: call " + std::to_string(i + 1) + " ok").c_str());
    }

    // === Cleanup ===
    HILOGI("=== Cleanup ===");
    respawnRecipient->Disable();
    client->Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
            KillAndWait(g_engine_pid);
            g_engine_pid = -1;
    }
    registry.Stop();

    HILOGI("=== DT crash recovery: ALL PASSED (restarts=%{public}d) ===",
          engineRestarts.load());
    return 0;
}
