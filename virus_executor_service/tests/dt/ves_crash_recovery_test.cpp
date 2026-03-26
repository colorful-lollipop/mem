#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "client/ves_client.h"
#include "client/internal/ves_client_recovery_access.h"
#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/registry_server.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_types.h"

namespace {

constexpr uint32_t RECOVERY_PROBE_EXEC_TIMEOUT_MS = 500;
constexpr int kRepeatedExternalKillCycles = 8;
constexpr int kAllowedFdGrowth = 2;
constexpr int kConcurrentScanThreads = 4;

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_it_registry_" + std::to_string(getpid()) + ".sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_it_service_" + std::to_string(getpid()) + ".sock";

std::mutex g_engine_mutex;
std::atomic<pid_t> g_engine_pid{-1};

class RecoveryEventObserver {
public:
    void Attach(VirusExecutorService::VesClient& client)
    {
        VirusExecutorService::internal::VesClientRecoveryAccess::SetRecoveryEventCallback(
            client,
            [this](const MemRpc::RecoveryEventReport& report) {
                if (report.state == MemRpc::ClientLifecycleState::Cooldown ||
                    report.state == MemRpc::ClientLifecycleState::NoSession ||
                    (report.state == MemRpc::ClientLifecycleState::Recovering &&
                     report.previousState == MemRpc::ClientLifecycleState::Active)) {
                    sawFaultRecovery_.store(true, std::memory_order_release);
                }
            });
    }

    [[nodiscard]] bool SawFaultRecovery() const
    {
        return sawFaultRecovery_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> sawFaultRecovery_{false};
};

pid_t LoadEnginePid()
{
    return g_engine_pid.load(std::memory_order_relaxed);
}

void StoreEnginePid(pid_t pid)
{
    g_engine_pid.store(pid, std::memory_order_relaxed);
}

pid_t SpawnEngine(const std::string& enginePath)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(), REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(), nullptr);
        _exit(1);
    }
    return pid;
}

void KillAndWait(pid_t pid)
{
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

std::string EnginePathFromSelf()
{
    char buf[1024] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
        return "./VirusExecutorService";
    std::string self(buf, static_cast<size_t>(len));
    auto pos = self.rfind('/');
    if (pos == std::string::npos)
        return "./VirusExecutorService";
    return self.substr(0, pos) + "/VirusExecutorService";
}

class CleanupGuard {
public:
    explicit CleanupGuard(std::function<void()> cleanup)
        : cleanup_(std::move(cleanup))
    {
    }
    ~CleanupGuard()
    {
        if (cleanup_) {
            cleanup_();
        }
    }

    CleanupGuard(const CleanupGuard&) = delete;
    CleanupGuard& operator=(const CleanupGuard&) = delete;

private:
    std::function<void()> cleanup_;
};

bool WaitForLoadCountAdvance(std::atomic<int>* loadCount, int previousLoadCount, std::chrono::milliseconds timeout)
{
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

bool WaitForCondition(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool WaitForRecoveredScan(VirusExecutorService::VesClient* client,
                          const std::string& path,
                          int expectedThreatLevel,
                          VirusExecutorService::ScanFileReply* reply,
                          std::chrono::milliseconds timeout,
                          MemRpc::StatusCode* lastStatus)
{
    if (client == nullptr || reply == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    MemRpc::StatusCode status = MemRpc::StatusCode::InvalidArgument;
    VirusExecutorService::ScanTask task{path};
    while (std::chrono::steady_clock::now() < deadline) {
        status = client->ScanFile(task, reply, MemRpc::Priority::Normal, RECOVERY_PROBE_EXEC_TIMEOUT_MS);
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

int CountOpenFileDescriptors()
{
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }

    const int dirFd = dirfd(dir);
    int count = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry == nullptr) {
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (std::atoi(entry->d_name) == dirFd) {
            continue;
        }
        ++count;
    }
    closedir(dir);
    return count;
}

bool IsExpectedConcurrentScanStatus(MemRpc::StatusCode status)
{
    switch (status) {
        case MemRpc::StatusCode::Ok:
        case MemRpc::StatusCode::PeerDisconnected:
        case MemRpc::StatusCode::CrashedDuringExecution:
        case MemRpc::StatusCode::ExecTimeout:
        case MemRpc::StatusCode::ClientClosed:
            return true;
        default:
            return false;
    }
}

void CleanupSocketFiles()
{
    unlink(REGISTRY_SOCKET.c_str());
    unlink(SERVICE_SOCKET.c_str());
}

}  // namespace

TEST(VesCrashRecoveryTest, CrashThenRecover)
{
    CleanupSocketFiles();
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID)
            return false;
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
        if (spawnedPid < 0)
            return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID)
            return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VirusExecutorService::VesClientOptions options;
    options.recoveryPolicy.onFailure = [](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
    };
    auto client = VirusExecutorService::VesClient::Connect(options);
    ASSERT_NE(client, nullptr);
    CleanupGuard cleanup([&]() {
        if (client != nullptr) {
            client->Shutdown();
        }
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
        CleanupSocketFiles();
    });

    VirusExecutorService::ScanTask cleanTask{"/data/clean.apk"};
    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);

    const pid_t crashedPid = LoadEnginePid();

    ASSERT_EQ(kill(crashedPid, SIGKILL), 0);
    ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5)));
    client->Shutdown();
    client = VirusExecutorService::VesClient::Connect(options);
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/clean_after.apk"}, &reply),
              MemRpc::StatusCode::Ok);
    ASSERT_EQ(reply.threatLevel, 0);
}

TEST(VesCrashRecoveryTest, CrashThenRecoverWithoutRecreatingClient)
{
    CleanupSocketFiles();
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID)
            return false;
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
        if (spawnedPid < 0)
            return false;
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID)
            return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VirusExecutorService::VesClientOptions options;
    options.recoveryPolicy.onFailure = [](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
    };
    auto client = VirusExecutorService::VesClient::Connect(options);
    ASSERT_NE(client, nullptr);
    RecoveryEventObserver observer;
    CleanupGuard cleanup([&]() {
        client->Shutdown();
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
        CleanupSocketFiles();
    });

    observer.Attach(*client);

    VirusExecutorService::ScanTask cleanTask{"/data/clean_same_client.apk"};
    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);
    ASSERT_EQ(reply.threatLevel, 0);

    const pid_t crashedPid = LoadEnginePid();
    const int previousLoadCount = loadCount.load();
    const auto* originalClient = client.get();

    ASSERT_EQ(kill(crashedPid, SIGKILL), 0);
    ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5)));
    ASSERT_TRUE(WaitForCondition([&]() { return observer.SawFaultRecovery(); }, std::chrono::seconds(2)));

    MemRpc::StatusCode lastStatus = MemRpc::StatusCode::InvalidArgument;
    ASSERT_TRUE(WaitForRecoveredScan(client.get(),
                                     "/data/clean_after_same_client.apk",
                                     0,
                                     &reply,
                                     std::chrono::seconds(5),
                                     &lastStatus))
        << "last status=" << static_cast<int>(lastStatus);
    EXPECT_EQ(client.get(), originalClient);
    EXPECT_TRUE(observer.SawFaultRecovery());
    EXPECT_TRUE(WaitForLoadCountAdvance(&loadCount, previousLoadCount, std::chrono::seconds(2)));

    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/virus_after_same_client.apk"}, &reply),
              MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 1);
}

TEST(VesCrashRecoveryTest, RepeatedExternalKillsRecoverAndKeepFdCountStable)
{
    CleanupSocketFiles();
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return false;
        }
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
        if (spawnedPid < 0) {
            return false;
        }
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VirusExecutorService::VesClientOptions options;
    options.recoveryPolicy.onFailure = [](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
    };
    auto client = VirusExecutorService::VesClient::Connect(options);
    ASSERT_NE(client, nullptr);
    CleanupGuard cleanup([&]() {
        client->Shutdown();
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
        CleanupSocketFiles();
    });

    VirusExecutorService::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/fd_baseline.apk"}, &reply), MemRpc::StatusCode::Ok);
    ASSERT_EQ(reply.threatLevel, 0);

    const int baselineFdCount = CountOpenFileDescriptors();
    ASSERT_GE(baselineFdCount, 0);

    for (int cycle = 0; cycle < kRepeatedExternalKillCycles; ++cycle) {
        const pid_t crashedPid = LoadEnginePid();
        ASSERT_GT(crashedPid, 0) << "cycle " << cycle;
        const int previousLoadCount = loadCount.load();

        ASSERT_EQ(kill(crashedPid, SIGKILL), 0) << "cycle " << cycle;
        ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5))) << "cycle " << cycle;
        ASSERT_TRUE(WaitForLoadCountAdvance(&loadCount, previousLoadCount, std::chrono::seconds(2))) << "cycle " << cycle;

        MemRpc::StatusCode lastStatus = MemRpc::StatusCode::InvalidArgument;
        ASSERT_TRUE(WaitForRecoveredScan(client.get(),
                                         "/data/recovered_after_external_kill_" + std::to_string(cycle) + ".apk",
                                         0,
                                         &reply,
                                         std::chrono::seconds(5),
                                         &lastStatus))
            << "cycle " << cycle << " last status=" << static_cast<int>(lastStatus);

        EXPECT_EQ(reply.threatLevel, 0) << "cycle " << cycle;

        const int currentFdCount = CountOpenFileDescriptors();
        ASSERT_GE(currentFdCount, 0) << "cycle " << cycle;
        EXPECT_LE(currentFdCount, baselineFdCount + kAllowedFdGrowth)
            << "cycle " << cycle << " baseline=" << baselineFdCount << " current=" << currentFdCount;
    }
}

TEST(VesCrashRecoveryTest, RepeatedExternalKillsDuringConcurrentScanTrafficStillRecover)
{
    CleanupSocketFiles();
    const std::string enginePath = EnginePathFromSelf();
    std::atomic<int> loadCount{0};

    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return false;
        }
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
        if (spawnedPid < 0) {
            return false;
        }
        loadCount.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
    });

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        StoreEnginePid(SpawnEngine(enginePath));
    }
    ASSERT_GT(LoadEnginePid(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VirusExecutorService::VesClientOptions options;
    options.recoveryPolicy.onFailure = [](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
    };
    auto client = VirusExecutorService::VesClient::Connect(options);
    ASSERT_NE(client, nullptr);
    CleanupGuard cleanup([&]() {
        client->Shutdown();
        registry.Stop();
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(LoadEnginePid());
        StoreEnginePid(-1);
        CleanupSocketFiles();
    });

    VirusExecutorService::ScanFileReply warmupReply;
    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/concurrent_warmup.apk"}, &warmupReply),
              MemRpc::StatusCode::Ok);
    ASSERT_EQ(warmupReply.threatLevel, 0);

    std::atomic<bool> stopWorkers{false};
    std::atomic<int> okCount{0};
    std::atomic<int> peerDisconnectedCount{0};
    std::atomic<int> crashedCount{0};
    std::atomic<int> execTimeoutCount{0};
    std::atomic<int> clientClosedCount{0};
    std::atomic<int> unexpectedCount{0};
    std::atomic<int> firstUnexpectedStatus{-1};

    std::vector<std::thread> workers;
    workers.reserve(kConcurrentScanThreads);
    for (int worker = 0; worker < kConcurrentScanThreads; ++worker) {
        workers.emplace_back([&, worker]() {
            int scanIndex = 0;
            while (!stopWorkers.load(std::memory_order_acquire)) {
                VirusExecutorService::ScanFileReply reply;
                const std::string path = "/data/concurrent_kill_worker_" + std::to_string(worker) + "_" +
                                         std::to_string(scanIndex++) + ".apk";
                const MemRpc::StatusCode status =
                    client->ScanFile(VirusExecutorService::ScanTask{path},
                                     &reply,
                                     MemRpc::Priority::Normal,
                                     RECOVERY_PROBE_EXEC_TIMEOUT_MS);
                switch (status) {
                    case MemRpc::StatusCode::Ok:
                        ++okCount;
                        break;
                    case MemRpc::StatusCode::PeerDisconnected:
                        ++peerDisconnectedCount;
                        break;
                    case MemRpc::StatusCode::CrashedDuringExecution:
                        ++crashedCount;
                        break;
                    case MemRpc::StatusCode::ExecTimeout:
                        ++execTimeoutCount;
                        break;
                    case MemRpc::StatusCode::ClientClosed:
                        ++clientClosedCount;
                        break;
                    default:
                        ++unexpectedCount;
                        {
                            int expected = -1;
                            firstUnexpectedStatus.compare_exchange_strong(
                                expected, static_cast<int>(status), std::memory_order_acq_rel);
                        }
                        break;
                }
                if (!IsExpectedConcurrentScanStatus(status)) {
                    break;
                }
            }
        });
    }

    ASSERT_TRUE(WaitForCondition([&]() { return okCount.load(std::memory_order_acquire) >= kConcurrentScanThreads; },
                                 std::chrono::seconds(3)));

    const int baselineFdCount = CountOpenFileDescriptors();
    ASSERT_GE(baselineFdCount, 0);

    for (int cycle = 0; cycle < kRepeatedExternalKillCycles; ++cycle) {
        const int previousOkCount = okCount.load(std::memory_order_acquire);
        const int previousLoadCount = loadCount.load();
        const pid_t crashedPid = LoadEnginePid();
        ASSERT_GT(crashedPid, 0) << "cycle " << cycle;

        ASSERT_EQ(kill(crashedPid, SIGKILL), 0) << "cycle " << cycle;
        ASSERT_TRUE(WaitForEngineExit(crashedPid, std::chrono::seconds(5))) << "cycle " << cycle;
        ASSERT_TRUE(WaitForLoadCountAdvance(&loadCount, previousLoadCount, std::chrono::seconds(2))) << "cycle " << cycle;
        ASSERT_TRUE(WaitForCondition([&]() { return okCount.load(std::memory_order_acquire) > previousOkCount; },
                                     std::chrono::seconds(5)))
            << "cycle " << cycle;

        const int currentFdCount = CountOpenFileDescriptors();
        ASSERT_GE(currentFdCount, 0) << "cycle " << cycle;
        EXPECT_LE(currentFdCount, baselineFdCount + kAllowedFdGrowth)
            << "cycle " << cycle << " baseline=" << baselineFdCount << " current=" << currentFdCount;
    }

    stopWorkers.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(unexpectedCount.load(std::memory_order_acquire), 0)
        << "first unexpected status=" << firstUnexpectedStatus.load(std::memory_order_acquire);
    EXPECT_GT(okCount.load(std::memory_order_acquire), 0);
    EXPECT_GT(loadCount.load(), 0);

    VirusExecutorService::ScanFileReply finalReply;
    ASSERT_EQ(client->ScanFile(VirusExecutorService::ScanTask{"/data/concurrent_kill_final.apk"}, &finalReply),
              MemRpc::StatusCode::Ok);
    EXPECT_EQ(finalReply.threatLevel, 0);
}
