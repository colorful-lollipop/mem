#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "iservice_registry.h"
#include "transport/registry_backend.h"
#include "transport/registry_server.h"
#include "transport/ves_bootstrap_interface.h"
#include "client/ves_client.h"
#include "ves/ves_sample_rules.h"
#include "ves/ves_types.h"
#include "virus_protection_service_log.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/virus_executor_service_stress_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/virus_executor_service_stress_service.sock";

volatile std::sig_atomic_t g_stop = 0;
std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

void SignalHandler(int) {
    g_stop = 1;
}

const std::vector<std::string> kNormalTokens = {
    "clean", "virus", "eicar", "sleep10",
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

void RespawnEngine(const std::string& enginePath, StressStats* stats) {
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

void WorkerThread(const StressConfig& config, uint32_t threadId,
                  VirusExecutorService::VesClient* client, StressStats* stats) {
    std::mt19937 rng(config.seed + threadId);
    std::uniform_int_distribution<size_t> tokenDist(0, kNormalTokens.size() - 1);
    std::uniform_real_distribution<double> crashDist(0.0, 1.0);

    for (int i = 0; i < config.iterations && !g_stop; i++) {
        bool doCrash = config.enableCrash && crashDist(rng) < kCrashProbability;
        std::string token = doCrash ? "crash" : kNormalTokens[tokenDist(rng)];
        std::string path = "/data/stress_" + token + "_" +
                           std::to_string(threadId) + "_" + std::to_string(i) + ".apk";

        const auto behavior = VirusExecutorService::EvaluateSamplePath(path);

        if (doCrash) {
            stats->crashesSent++;
        }
        stats->sleepMs += behavior.sleepMs;

        VirusExecutorService::ScanFileReply reply;
        auto status = client->ScanFile(path, &reply);
        stats->total++;

        if (status != MemRpc::StatusCode::Ok) {
            stats->rpcError++;
            continue;
        }

        if (reply.threatLevel != behavior.threatLevel) {
            stats->mismatch++;
            HILOGE("thread %{public}u: MISMATCH %{public}s expected=%{public}d got=%{public}d",
                  threadId, path.c_str(), behavior.threatLevel, reply.threatLevel);
        } else {
            stats->ok++;
        }
    }
}

StressConfig ParseArgs(int argc, char* argv[]) {
    StressConfig config;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.threads = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            config.iterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            config.seed = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-crash") == 0) {
            config.enableCrash = false;
        }
    }
    return config;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    StressConfig config = ParseArgs(argc, argv);

    HILOGI("stress client: threads=%{public}d iterations=%{public}d seed=%{public}u crash=%{public}s",
          config.threads, config.iterations, config.seed,
          config.enableCrash ? "5%" : "off");

    // Determine engine path relative to our binary.
    std::string dir = ".";
    std::string argv0(argv[0]);
    auto pos = argv0.rfind('/');
    if (pos != std::string::npos) {
        dir = argv0.substr(0, pos);
    }
    std::string enginePath = dir + "/VirusExecutorService";

    // Self-host registry server.
    VirusExecutorService::RegistryServer registry(REGISTRY_SOCKET);

    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != VirusExecutorService::VES_SA_ID) {
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
        if (sa_id != VirusExecutorService::VES_SA_ID) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        HILOGI("unloading engine SA (pid=%{public}d)", g_engine_pid);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    if (!registry.Start()) {
        HILOGE("failed to start registry");
        return 1;
    }
    HILOGI("registry started at %{public}s", REGISTRY_SOCKET.c_str());

    // Inject backend for client-side SAM access.
    auto backend = std::make_shared<VirusExecutorService::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    VirusExecutorService::VesClient::RegisterProxyFactory();

    // Spawn engine.
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    if (g_engine_pid < 0) {
        HILOGE("failed to spawn engine");
        registry.Stop();
        return 1;
    }
    HILOGI("engine spawned pid=%{public}d", g_engine_pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Create a single shared client (engine supports one session).
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(VirusExecutorService::VES_SA_ID, 5000);
    if (remote == nullptr) {
        HILOGE("LoadSystemAbility failed");
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
        registry.Stop();
        return 1;
    }

    StressStats stats;

    auto client = std::make_unique<VirusExecutorService::VesClient>(remote);
    client->SetEngineRestartCallback([&]() {
        RespawnEngine(enginePath, &stats);
    });
    if (client->Init() != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient init failed");
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
        registry.Stop();
        return 1;
    }

    // Run worker threads sharing the single client.
    std::vector<std::thread> workers;
    workers.reserve(config.threads);

    const auto startTime = std::chrono::steady_clock::now();

    for (int t = 0; t < config.threads; t++) {
        workers.emplace_back(WorkerThread, std::cref(config),
                             static_cast<uint32_t>(t), client.get(), &stats);
    }

    for (auto& w : workers) {
        w.join();
    }

    const auto endTime = std::chrono::steady_clock::now();
    const auto actualMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    const uint64_t estimatedMs = stats.sleepMs.load();

    HILOGI("=== Stress Results ===");
    HILOGI("total=%{public}d ok=%{public}d mismatch=%{public}d rpc_error=%{public}d",
          stats.total.load(), stats.ok.load(), stats.mismatch.load(), stats.rpcError.load());
    HILOGI("crashes_sent=%{public}d engine_restarts=%{public}d",
          stats.crashesSent.load(), stats.engineRestarts.load());
    HILOGI("estimated=%{public}llu ms (sleep_total=%{public}llu ms / server_workers=1)  actual=%{public}llu ms",
          static_cast<unsigned long long>(estimatedMs),
          static_cast<unsigned long long>(stats.sleepMs.load()),
          static_cast<unsigned long long>(actualMs));

    // Cleanup.
    client->Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
    registry.Stop();

    // With crash enabled, RPC errors are expected (in-flight requests fail on engine death).
    // Only mismatch (wrong result) is a real failure.
    int exitCode = (stats.mismatch.load() == 0) ? 0 : 1;
    HILOGI("stress client exit=%{public}d", exitCode);
    return exitCode;
}
