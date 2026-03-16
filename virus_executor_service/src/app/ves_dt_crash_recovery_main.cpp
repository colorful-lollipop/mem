// DT: crash recovery
//
// Step 1: normal scan → verify ok
// Step 2: force-kill engine → engine dies
// Step 3: verify the crashed engine process exits
// Step 4: next client request triggers SA reload + engine restart
// Step 5: normal scan again → verify ok
//
// Uses the supervisor pattern: self-host registry, fork engine, VesClient.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

bool WaitForEngineExit(pid_t pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            std::lock_guard<std::mutex> lock(g_engine_mutex);
            if (g_engine_pid == pid) {
                g_engine_pid = -1;
            }
            return true;
        }
        if (result < 0) {
            if (errno == ECHILD) {
                std::lock_guard<std::mutex> lock(g_engine_mutex);
                if (g_engine_pid == pid) {
                    g_engine_pid = -1;
                }
                return true;
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool WaitForLoadCountAdvance(
    std::atomic<int>* loadCount,
    int previousLoadCount,
    std::chrono::milliseconds timeout) {
    if (loadCount == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (loadCount->load() > previousLoadCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return loadCount->load() > previousLoadCount;
}

bool WaitForCondition(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool WaitForRecoveredScan(
    VirusExecutorService::VesClient* client,
    const std::string& path,
    int expectedThreatLevel,
    VirusExecutorService::ScanFileReply* reply,
    std::chrono::milliseconds timeout,
    MemRpc::StatusCode* lastStatus) {
    if (client == nullptr || reply == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    MemRpc::StatusCode status = MemRpc::StatusCode::InvalidArgument;
    VirusExecutorService::ScanTask task{path};
    while (std::chrono::steady_clock::now() < deadline) {
        status = client->ScanFile(task, reply);
        if (status == MemRpc::StatusCode::Ok && reply->threatLevel == expectedThreatLevel) {
            if (lastStatus != nullptr) {
                *lastStatus = status;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (lastStatus != nullptr) {
        *lastStatus = status;
    }
    return false;
}

class CleanupGuard {
public:
    explicit CleanupGuard(std::function<void()> cleanup) : cleanup_(std::move(cleanup)) {}
    ~CleanupGuard() {
        if (cleanup_) {
            cleanup_();
        }
    }

    CleanupGuard(const CleanupGuard&) = delete;
    CleanupGuard& operator=(const CleanupGuard&) = delete;

private:
    std::function<void()> cleanup_;
};

#define DT_CHECK(expr, msg)                              \
    do {                                                 \
        if (!(expr)) {                                   \
            HILOGE("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); \
            return 1;                                    \
        }                                                \
        HILOGI("PASS: %s", msg);                         \
    } while (0)

}  // namespace

int main([[maybe_unused]] int argc, char* argv[]) {
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
    std::atomic<int> loadCount{0};

    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            int status = 0;
            const pid_t result = waitpid(g_engine_pid, &status, WNOHANG);
            if (result == 0) {
                return true;
            }
            g_engine_pid = -1;
        }
        HILOGI("load callback: spawning engine");
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        loadCount.fetch_add(1);
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

    VirusExecutorService::VesClientOptions options;
    options.execTimeoutRestartDelayMs = 0;
    options.engineDeathRestartDelayMs = 0;
    auto client = std::make_unique<VirusExecutorService::VesClient>(remote, options);
    CleanupGuard cleanup([&]() {
        if (client != nullptr) {
            client->Shutdown();
        }
        {
            std::lock_guard<std::mutex> lock(g_engine_mutex);
            KillAndWait(g_engine_pid);
            g_engine_pid = -1;
        }
        registry.Stop();
    });

    auto initStatus = client->Init();
    DT_CHECK(initStatus == MemRpc::StatusCode::Ok, "VesClient Init ok");

    // === Step 1: normal scan ===
    HILOGI("=== Step 1: normal scan ===");
    {
        VirusExecutorService::ScanFileReply reply;
        VirusExecutorService::ScanTask cleanTask{"/data/dt_clean.apk"};
        auto status = client->ScanFile(cleanTask, &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step1: clean scan RPC ok");
        DT_CHECK(reply.threatLevel == 0, "step1: clean file threat=0");
    }
    {
        VirusExecutorService::ScanFileReply reply;
        VirusExecutorService::ScanTask virusTask{"/data/dt_virus.apk"};
        auto status = client->ScanFile(virusTask, &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step1: virus scan RPC ok");
        DT_CHECK(reply.threatLevel == 1, "step1: virus file threat=1");
    }

    // === Step 2: force-kill engine ===
    HILOGI("=== Step 2: force-kill engine ===");
    const pid_t firstCrashPid = g_engine_pid;
    const int firstRecoveryLoadCount = loadCount.load();
    {
        DT_CHECK(kill(firstCrashPid, SIGKILL) == 0, "step2: engine kill sent");
    }

    // === Step 3: wait for the crashed process to exit ===
    HILOGI("=== Step 3: waiting for engine death ===");
    {
        DT_CHECK(WaitForEngineExit(firstCrashPid, std::chrono::seconds(10)),
                 "step3: engine process exited");
    }

    // === Step 4: same client internally reloads SA and recovers ===
    HILOGI("=== Step 4: same-client recovery after crash ===");
    {
        VirusExecutorService::ScanFileReply reply;
        MemRpc::StatusCode lastStatus = MemRpc::StatusCode::InvalidArgument;
        DT_CHECK(WaitForCondition([&]() { return client->EngineDied(); },
                                  std::chrono::seconds(2)),
                 "step4: engine death callback observed");
        DT_CHECK(WaitForRecoveredScan(client.get(),
                                      "/data/dt_clean_after.apk",
                                      0,
                                      &reply,
                                      std::chrono::seconds(5),
                                      &lastStatus),
                 "step4: post-recovery clean scan ok");
        DT_CHECK(reply.threatLevel == 0, "step4: clean file threat=0");
        const bool reloaded = WaitForLoadCountAdvance(
            &loadCount, firstRecoveryLoadCount, std::chrono::seconds(2));
        DT_CHECK(reloaded, "step4: same client triggered SA reload");
    }
    {
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile(VirusExecutorService::ScanTask{"/data/dt_virus_after.apk"}, &reply);
        DT_CHECK(status == MemRpc::StatusCode::Ok, "step4: post-recovery virus scan ok");
        DT_CHECK(reply.threatLevel == 1, "step4: virus file threat=1");
    }

    // === Step 5: second crash + 10 sequential calls (卡住定位) ===
    HILOGI("=== Step 5: second crash + 10 sequential normal calls ===");
    const pid_t secondCrashPid = g_engine_pid;
    const int secondRecoveryLoadCount = loadCount.load();
    {
        HILOGI("step5: killing engine...");
        DT_CHECK(kill(secondCrashPid, SIGKILL) == 0, "step5: second engine kill sent");
    }
    DT_CHECK(WaitForEngineExit(secondCrashPid, std::chrono::seconds(10)),
             "step5: second engine process exited");
    {
        VirusExecutorService::ScanFileReply reply;
        MemRpc::StatusCode lastStatus = MemRpc::StatusCode::InvalidArgument;
        DT_CHECK(WaitForRecoveredScan(client.get(),
                                     "/data/dt_post_second_recovery_probe.apk",
                                      0,
                                      &reply,
                                      std::chrono::seconds(5),
                                      &lastStatus),
                 "step5: same client recovered after second crash");
    }
    const bool reloaded = WaitForLoadCountAdvance(
        &loadCount, secondRecoveryLoadCount, std::chrono::seconds(2));
    DT_CHECK(reloaded, "step5: second same-client SA reload observed");

    for (int i = 0; i < 10; i++) {
        HILOGI("step5: call %{public}d/10 ...", i + 1);
        VirusExecutorService::ScanFileReply reply;
        const auto t0 = std::chrono::steady_clock::now();
        VirusExecutorService::ScanTask task{
            "/data/dt_post_recover_" + std::to_string(i) + ".apk"};
        auto status = client->ScanFile(task, &reply);
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

    HILOGI("=== DT crash recovery: ALL PASSED (restarts=%{public}d) ===",
          loadCount.load());
    return 0;
}
