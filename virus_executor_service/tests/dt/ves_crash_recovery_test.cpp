#include <gtest/gtest.h>

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

namespace {

constexpr uint32_t RECOVERY_PROBE_EXEC_TIMEOUT_MS = 500;

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_it_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_it_service.sock";

std::mutex g_engine_mutex;
std::atomic<pid_t> g_engine_pid{-1};

pid_t LoadEnginePid()
{
    return g_engine_pid.load(std::memory_order_relaxed);
}

void StoreEnginePid(pid_t pid)
{
    g_engine_pid.store(pid, std::memory_order_relaxed);
}

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
            if (LoadEnginePid() == pid) {
                StoreEnginePid(-1);
            }
            return true;
        }
        if (result < 0) {
            if (errno == ECHILD) {
                std::lock_guard<std::mutex> lock(g_engine_mutex);
                if (LoadEnginePid() == pid) {
                    StoreEnginePid(-1);
                }
                return true;
            }
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
        status = client->ScanFile(task, reply, MemRpc::Priority::Normal,
                                  RECOVERY_PROBE_EXEC_TIMEOUT_MS);
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

}  // namespace

TEST(VesCrashRecoveryTest, CrashThenRecover) {
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const pid_t currentPid = LoadEnginePid();
        if (currentPid > 0) {
            int status = 0;
            const pid_t result = waitpid(currentPid, &status, WNOHANG);
            if (result == 0) {
                return true;
            }
            StoreEnginePid(-1);
        }
        const pid_t spawnedPid = SpawnEngine(enginePath);
        StoreEnginePid(spawnedPid);
        if (spawnedPid < 0) return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
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
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_EQ(client->Init(), MemRpc::StatusCode::Ok);

    VirusExecutorService::ScanTask cleanTask{"/data/clean.apk"};
    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);

    const pid_t crashedPid = LoadEnginePid();

    ASSERT_EQ(kill(crashedPid, SIGKILL), 0);
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

TEST(VesCrashRecoveryTest, CrashThenRecoverWithoutRecreatingClient) {
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const pid_t currentPid = LoadEnginePid();
        if (currentPid > 0) {
            int status = 0;
            const pid_t result = waitpid(currentPid, &status, WNOHANG);
            if (result == 0) {
                return true;
            }
            StoreEnginePid(-1);
        }
        const pid_t spawnedPid = SpawnEngine(enginePath);
        StoreEnginePid(spawnedPid);
        if (spawnedPid < 0) return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VES_CONTROL_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_CONTROL_SA_ID, 5000);
    ASSERT_NE(remote, nullptr);

    VirusExecutorService::VesClientOptions options;
    options.execTimeoutRestartDelayMs = 0;
    options.engineDeathRestartDelayMs = 0;
    VirusExecutorService::VesClient client(remote, options);
    CleanupGuard cleanup([&]() {
        client.Shutdown();
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    VirusExecutorService::ScanTask cleanTask{"/data/clean_same_client.apk"};
    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client.ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);
    ASSERT_EQ(reply.threatLevel, 0);

    const pid_t crashedPid = LoadEnginePid();
    const int previousLoadCount = loadCount.load();
    const auto* originalClient = &client;

    ASSERT_EQ(kill(crashedPid, SIGKILL), 0);
    ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5)));
    ASSERT_TRUE(WaitForCondition([&]() { return client.EngineDied(); },
                                 std::chrono::seconds(2)));

    MemRpc::StatusCode lastStatus = MemRpc::StatusCode::InvalidArgument;
    ASSERT_TRUE(WaitForRecoveredScan(&client,
                                     "/data/clean_after_same_client.apk",
                                     0,
                                     &reply,
                                     std::chrono::seconds(5),
                                     &lastStatus))
        << "last status=" << static_cast<int>(lastStatus);
    EXPECT_EQ(&client, originalClient);
    EXPECT_TRUE(client.EngineDied());
    EXPECT_TRUE(WaitForLoadCountAdvance(&loadCount,
                                        previousLoadCount,
                                        std::chrono::seconds(2)));

    ASSERT_EQ(client.ScanFile(VirusExecutorService::ScanTask{"/data/virus_after_same_client.apk"},
                              &reply),
              MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 1);
}
