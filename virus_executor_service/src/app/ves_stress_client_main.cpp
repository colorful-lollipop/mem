#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "client/ves_client.h"
#include "iremote_object.h"
#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/registry_server.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_sample_rules.h"
#include "ves/ves_types.h"
#include "virus_protection_executor_log.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_stress_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_stress_service.sock";

volatile std::sig_atomic_t g_stop = 0;
std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

void SignalHandler(int)
{
    g_stop = 1;
}

const std::vector<std::string> kNormalTokens = {
    "clean",
    "virus",
    "eicar",
    "sleep10",
};

constexpr double kCrashProbability = 0.05;

struct StressConfig {
    int threads = 2;
    int iterations = 4000;
    uint32_t seed = 1;
    bool enableCrash = true;
};

struct StressStats {
    std::atomic<int> total{0};
    std::atomic<int> mismatch{0};
    std::atomic<int> rpcError{0};
    std::atomic<int> ok{0};
    std::atomic<int> crashesSent{0};
    std::atomic<int> engineRestarts{0};
    std::atomic<uint64_t> sleepMs{0};
};

bool ParseIntArg(const char* text, int* value)
{
    if (text == nullptr || value == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || (end != nullptr && *end != '\0') || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool ParseUint32Arg(const char* text, uint32_t* value)
{
    if (text == nullptr || value == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || (end != nullptr && *end != '\0') ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

pid_t SpawnEngine(const std::string& enginePath)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(), REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(), nullptr);
        HILOGE("exec engine failed");
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

void RespawnEngine(const std::string& enginePath, StressStats* stats)
{
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    // Reap the dead engine process.
    if (g_engine_pid > 0) {
        int status = 0;
        waitpid(g_engine_pid, &status, WNOHANG);
        g_engine_pid = -1;
    }
    g_engine_pid = SpawnEngine(enginePath);
    if (g_engine_pid > 0) {
        stats->engineRestarts++;
        HILOGI("engine respawned pid=%{public}d", g_engine_pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

class EngineRespawnRecipient : public OHOS::IRemoteObject::DeathRecipient {
public:
    EngineRespawnRecipient(const std::string& enginePath, StressStats* stats)
        : enginePath_(enginePath),
          stats_(stats)
    {
    }

    void Disable()
    {
        enabled_.store(false);
    }

    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>&) override
    {
        if (!enabled_.load()) {
            return;
        }
        RespawnEngine(enginePath_, stats_);
    }

private:
    std::string enginePath_;
    StressStats* stats_;
    std::atomic<bool> enabled_{true};
};

void WorkerThread(const StressConfig& config,
                  uint32_t threadId,
                  VirusExecutorService::VesClient* client,
                  StressStats* stats)
{
    std::mt19937 rng(config.seed + threadId);
    std::uniform_int_distribution<size_t> tokenDist(0, kNormalTokens.size() - 1);
    std::uniform_real_distribution<double> crashDist(0.0, 1.0);

    for (int i = 0; i < config.iterations && !g_stop; i++) {
        bool doCrash = config.enableCrash && crashDist(rng) < kCrashProbability;
        std::string token = doCrash ? "crash" : kNormalTokens[tokenDist(rng)];
        std::string path = "/data/stress_" + token + "_" + std::to_string(threadId) + "_" + std::to_string(i) + ".apk";

        const auto behavior = VirusExecutorService::EvaluateSamplePath(path);

        if (doCrash) {
            stats->crashesSent++;
        }
        stats->sleepMs += behavior.sleepMs;

        VirusExecutorService::ScanTask task{path};
        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile(task, &reply);
        stats->total++;

        if (status != MemRpc::StatusCode::Ok) {
            stats->rpcError++;
            continue;
        }

        if (reply.threatLevel != behavior.threatLevel) {
            stats->mismatch++;
            HILOGE("thread %{public}u: MISMATCH %{public}s expected=%{public}d got=%{public}d",
                   threadId,
                   path.c_str(),
                   behavior.threatLevel,
                   reply.threatLevel);
        } else {
            stats->ok++;
        }
    }
}

StressConfig ParseArgs(int argc, char* argv[])
{
    StressConfig config;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            if (!ParseIntArg(value, &config.threads)) {
                HILOGW("invalid --threads value: %{public}s", value);
            }
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            if (!ParseIntArg(value, &config.iterations)) {
                HILOGW("invalid --iterations value: %{public}s", value);
            }
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            if (!ParseUint32Arg(value, &config.seed)) {
                HILOGW("invalid --seed value: %{public}s", value);
            }
        } else if (std::strcmp(argv[i], "--no-crash") == 0) {
            config.enableCrash = false;
        }
    }
    return config;
}

std::string ResolveBinaryDir(const char* argv0)
{
    std::string dir = ".";
    std::string programPath(argv0);
    const auto pos = programPath.rfind('/');
    if (pos != std::string::npos) {
        dir = programPath.substr(0, pos);
    }
    return dir;
}

void ConfigureRegistry(VirusExecutorService::RegistryServer& registry, const std::string& enginePath)
{
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            return true;
        }
        HILOGI("spawning engine SA for load request");
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    });

    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        HILOGI("unloading engine SA (pid=%{public}d)", g_engine_pid);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });
}

bool StartRegistry(VirusExecutorService::RegistryServer& registry, const std::string& enginePath)
{
    ConfigureRegistry(registry, enginePath);
    if (!registry.Start()) {
        HILOGE("failed to start registry");
        return false;
    }
    HILOGI("registry started at %{public}s", REGISTRY_SOCKET.c_str());
    return true;
}

bool StartEngine(const std::string& enginePath, VirusExecutorService::RegistryServer& registry)
{
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    if (g_engine_pid < 0) {
        HILOGE("failed to spawn engine");
        registry.Stop();
        return false;
    }
    HILOGI("engine spawned pid=%{public}d", g_engine_pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return true;
}

OHOS::sptr<OHOS::IRemoteObject> LoadEngineRemote(VirusExecutorService::RegistryServer& registry)
{
    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VIRUS_PROTECTION_EXECUTOR_SA_ID, 5000);
    if (remote != nullptr) {
        return remote;
    }

    HILOGE("LoadSystemAbility failed");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();
    return nullptr;
}

std::unique_ptr<VirusExecutorService::VesClient> ConnectClient(VirusExecutorService::RegistryServer& registry)
{
    auto client = VirusExecutorService::VesClient::Connect();
    if (client != nullptr) {
        return client;
    }

    HILOGE("VesClient init failed");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();
    return nullptr;
}

bool InstallRespawnRecipient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                             const std::shared_ptr<EngineRespawnRecipient>& recipient,
                             VirusExecutorService::RegistryServer& registry)
{
    if (remote->AddDeathRecipient(recipient)) {
        return true;
    }

    HILOGE("failed to register DeathRecipient");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    KillAndWait(g_engine_pid);
    g_engine_pid = -1;
    registry.Stop();
    return false;
}

void RunWorkers(const StressConfig& config, VirusExecutorService::VesClient& client, StressStats* stats)
{
    std::vector<std::thread> workers;
    const size_t workerCount = config.threads > 0 ? static_cast<size_t>(config.threads) : 0U;
    workers.reserve(workerCount);
    for (int t = 0; t < config.threads; t++) {
        workers.emplace_back(WorkerThread, std::cref(config), static_cast<uint32_t>(t), &client, stats);
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

void LogStressResults(const StressStats& stats, uint64_t actualMs)
{
    const uint64_t estimatedMs = stats.sleepMs.load();
    HILOGI("=== Stress Results ===");
    HILOGI("total=%{public}d ok=%{public}d mismatch=%{public}d rpc_error=%{public}d",
           stats.total.load(),
           stats.ok.load(),
           stats.mismatch.load(),
           stats.rpcError.load());
    HILOGI("crashes_sent=%{public}d engine_restarts=%{public}d",
           stats.crashesSent.load(),
           stats.engineRestarts.load());
    HILOGI("estimated=%{public}llu ms (sleep_total=%{public}llu ms / server_workers=1)  actual=%{public}llu ms",
           static_cast<unsigned long long>(estimatedMs),
           static_cast<unsigned long long>(stats.sleepMs.load()),
           static_cast<unsigned long long>(actualMs));
}

void ShutdownStressClient(const std::shared_ptr<EngineRespawnRecipient>& recipient,
                          VirusExecutorService::VesClient& client,
                          VirusExecutorService::RegistryServer& registry)
{
    recipient->Disable();
    client.Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
    registry.Stop();
}

}  // namespace

int main(int argc, char* argv[])
{
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    StressConfig config = ParseArgs(argc, argv);

    HILOGI("stress client: threads=%{public}d iterations=%{public}d seed=%{public}u crash=%{public}s",
           config.threads,
           config.iterations,
           config.seed,
           config.enableCrash ? "5%" : "off");

    const std::string enginePath = ResolveBinaryDir(argv[0]) + "/VirusExecutorService";
    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);
    if (!StartRegistry(registry, enginePath)) {
        return 1;
    }
    if (!StartEngine(enginePath, registry)) {
        return 1;
    }

    auto remote = LoadEngineRemote(registry);
    if (remote == nullptr) {
        return 1;
    }

    StressStats stats;
    auto respawnRecipient = std::make_shared<EngineRespawnRecipient>(enginePath, &stats);
    if (!InstallRespawnRecipient(remote, respawnRecipient, registry)) {
        return 1;
    }

    auto client = ConnectClient(registry);
    if (client == nullptr) {
        return 1;
    }

    const auto startTime = std::chrono::steady_clock::now();
    RunWorkers(config, *client, &stats);
    const auto endTime = std::chrono::steady_clock::now();
    const auto actualMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

    LogStressResults(stats, actualMs);
    ShutdownStressClient(respawnRecipient, *client, registry);

    const int exitCode = (stats.mismatch.load() == 0) ? 0 : 1;
    HILOGI("stress client exit=%{public}d", exitCode);
    return exitCode;
}
