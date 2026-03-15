#include <gtest/gtest.h>

#include <atomic>
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

namespace {

constexpr uint32_t RECOVERY_PROBE_EXEC_TIMEOUT_MS = 500;

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_it_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_it_service.sock";

std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(),
              nullptr);
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

bool WaitForEngineExit(pid_t pid, std::chrono::milliseconds timeout) {
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
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string EnginePathFromSelf() {
    char buf[1024] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "./VirusExecutorService";
    std::string self(buf, static_cast<size_t>(len));
    auto pos = self.rfind('/');
    if (pos == std::string::npos) return "./VirusExecutorService";
    return self.substr(0, pos) + "/VirusExecutorService";
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

bool WaitForLoadCountAdvance(
    const OHOS::sptr<OHOS::ISystemAbilityManager>& sam,
    std::atomic<int>* loadCount,
    int previousLoadCount,
    std::chrono::milliseconds timeout) {
    if (sam == nullptr || loadCount == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (loadCount->load() > previousLoadCount) {
            return true;
        }
        (void)sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
        if (loadCount->load() > previousLoadCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return loadCount->load() > previousLoadCount;
}

bool WaitForRecoveredScan(
    VirusExecutorService::VesClient* client,
    const OHOS::sptr<OHOS::ISystemAbilityManager>& sam,
    const std::string& path,
    int expectedThreatLevel,
    VirusExecutorService::ScanFileReply* reply,
    std::chrono::milliseconds timeout,
    MemRpc::StatusCode* lastStatus) {
    if (client == nullptr || reply == nullptr || sam == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    MemRpc::StatusCode status = MemRpc::StatusCode::InvalidArgument;
    VirusExecutorService::ScanTask task{path};
    while (std::chrono::steady_clock::now() < deadline) {
        status = client->ScanFile(task, reply, MemRpc::Priority::Normal,
                                  RECOVERY_PROBE_EXEC_TIMEOUT_MS);
        if (status == MemRpc::StatusCode::Ok && reply->threatLevel == expectedThreatLevel) {
            if (lastStatus != nullptr) {
                *lastStatus = status;
            }
            return true;
        }
        (void)sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (lastStatus != nullptr) {
        *lastStatus = status;
    }
    return false;
}

}  // namespace

TEST(VesCrashRecoveryTest, CrashThenRecover) {
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
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
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    ASSERT_GT(g_engine_pid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
    ASSERT_NE(remote, nullptr);

    VirusExecutorService::VesClientOptions options;
    options.execTimeoutRestartDelayMs = 0;
    options.engineDeathRestartDelayMs = 0;
    auto client = std::make_unique<VirusExecutorService::VesClient>(remote, options);
    CleanupGuard cleanup([&]() {
        if (client != nullptr) {
            client->Shutdown();
        }
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    ASSERT_EQ(client->Init(), MemRpc::StatusCode::Ok);

    VirusExecutorService::ScanTask cleanTask{"/data/clean.apk"};
    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);

    const pid_t crashedPid = g_engine_pid;

    // Crash request
    VirusExecutorService::ScanTask crashTask{"/data/crash.apk"};
    (void)client->ScanFile(crashTask, &reply);
    ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5)));
    client->Shutdown();
    auto recoveredRemote = sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
    ASSERT_NE(recoveredRemote, nullptr);
    client = std::make_unique<VirusExecutorService::VesClient>(recoveredRemote, options);
    ASSERT_EQ(client->Init(), MemRpc::StatusCode::Ok);
    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/clean_after.apk"}, &reply),
              MemRpc::StatusCode::Ok);
    ASSERT_EQ(reply.threatLevel, 0);
}
