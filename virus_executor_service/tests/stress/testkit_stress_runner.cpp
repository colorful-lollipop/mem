#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/core/runtime_utils.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_codec.h"
#include "testkit/testkit_protocol.h"
#include "testkit/testkit_service.h"
#include "testkit_stress_config.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService::testkit {
namespace {

namespace Mem = ::MemRpc;

using VirusExecutorService::testkit::TestkitService;
using MemRpc::EchoRequest;
using MemRpc::EchoReply;

void CloseHandles(Mem::BootstrapHandles& handles) {
    if (handles.shmFd >= 0) close(handles.shmFd);
    if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0) close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
}

struct SharedState {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> okCount{0};
    std::atomic<uint64_t> failCount{0};
    std::atomic<uint64_t> lastOkMs{0};
    std::mutex errorMutex;
    std::string error;
};

void WritePidFile(pid_t childPid) {
    const char* path = std::getenv("MEMRPC_STRESS_PID_FILE");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    output << "parent_pid=" << static_cast<long long>(getpid()) << "\n";
    output << "child_pid=" << static_cast<long long>(childPid) << "\n";
}

void RunChild(Mem::BootstrapHandles handles, uint32_t threads) {
    Mem::RpcServer server;
    server.SetBootstrapHandles(handles);
    Mem::ServerOptions options;
    options.highWorkerThreads = threads;
    options.normalWorkerThreads = threads;
    server.SetOptions(options);

    TestkitService service;
    RegisterHandlersToServer(&service, &server);
    if (server.Start() != Mem::StatusCode::Ok) {
        HILOGE("testkit stress server start failed");
        std::_Exit(1);
    }
    server.Run();
}

enum class RpcKind { Echo, Add, Sleep };

RpcKind PickKind(std::mt19937_64& rng, const StressConfig& config) {
    const int total = std::max(0, config.echoWeight) + std::max(0, config.addWeight) +
                      std::max(0, config.sleepWeight);
    if (total <= 0) {
        return RpcKind::Echo;
    }
    std::uniform_int_distribution<int> dist(1, total);
    const int pick = dist(rng);
    if (pick <= config.echoWeight) {
        return RpcKind::Echo;
    }
    if (pick <= config.echoWeight + config.addWeight) {
        return RpcKind::Add;
    }
    return RpcKind::Sleep;
}

std::size_t PickPayloadSize(std::mt19937_64& rng, const StressConfig& config) {
    if (config.payloadSizes.empty()) {
        return 0;
    }
    std::uniform_int_distribution<std::size_t> dist(0, config.payloadSizes.size() - 1);
    return config.payloadSizes[dist(rng)];
}

Mem::Priority PickPriority(std::mt19937_64& rng, const StressConfig& config) {
    const int pct = std::max(0, std::min(100, config.highPriorityPct));
    std::uniform_int_distribution<int> dist(1, 100);
    return dist(rng) <= pct ? Mem::Priority::High : Mem::Priority::Normal;
}

bool InBurstWindow(uint64_t elapsedMs, const StressConfig& config) {
    if (config.burstIntervalMs <= 0 || config.burstDurationMs <= 0) {
        return false;
    }
    const uint64_t interval = static_cast<uint64_t>(config.burstIntervalMs);
    const uint64_t duration = static_cast<uint64_t>(config.burstDurationMs);
    return (elapsedMs % interval) < duration;
}

void RecordError(SharedState* state, const std::string& message) {
    if (state == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->errorMutex);
    if (state->error.empty()) {
        state->error = message;
    }
    state->stop.store(true);
}

bool RunStress(const StressConfig& config) {
    Mem::DevBootstrapConfig bootstrapConfig;
    bootstrapConfig.maxRequestBytes = config.maxRequestBytes;
    bootstrapConfig.maxResponseBytes = config.maxResponseBytes;

    auto bootstrap = std::make_shared<Mem::DevBootstrapChannel>(bootstrapConfig);
    Mem::BootstrapHandles handles{};
    if (bootstrap->OpenSession(handles) != Mem::StatusCode::Ok) {
        HILOGE("stress bootstrap open session failed");
        return false;
    }
    CloseHandles(handles);

    const Mem::BootstrapHandles serverHandles = bootstrap->serverHandles();
    const pid_t child = fork();
    if (child == 0) {
        RunChild(serverHandles, static_cast<uint32_t>(config.threads));
        return true;
    }
    if (child < 0) {
        HILOGE("stress fork failed");
        return false;
    }

    WritePidFile(child);

    Mem::RpcClient client(bootstrap);
    if (client.Init() != Mem::StatusCode::Ok) {
        HILOGE("stress client init failed");
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        return false;
    }

    SharedState state;
    state.lastOkMs.store(Mem::MonotonicNowMs64());

    const auto start = std::chrono::steady_clock::now();
    const auto warmupEnd = start + std::chrono::seconds(config.warmupSec);
    const auto end = warmupEnd + std::chrono::seconds(config.durationSec);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(config.threads));

    for (int i = 0; i < config.threads; ++i) {
        workers.emplace_back([&, i]() {
            const uint64_t workerSeedBase =
                config.seed != 0 ? config.seed : Mem::MonotonicNowMs64();
            std::mt19937_64 rng(workerSeedBase + static_cast<uint64_t>(i));
            while (!state.stop.load() && std::chrono::steady_clock::now() < end) {
                const uint64_t elapsedMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count());

                const RpcKind kind = PickKind(rng, config);
                const Mem::Priority priority = PickPriority(rng, config);

                Mem::StatusCode status = Mem::StatusCode::Ok;
                if (kind == RpcKind::Echo) {
                    const std::size_t size = PickPayloadSize(rng, config);
                    std::string text(size, 'x');
                    EchoRequest request{text};
                    EchoReply reply;
                    status = Mem::InvokeTypedSync(
                        &client, static_cast<Mem::Opcode>(TestkitOpcode::Echo), request, &reply,
                        priority);
                } else if (kind == RpcKind::Add) {
                    AddRequest request{static_cast<int32_t>(rng()), static_cast<int32_t>(rng())};
                    AddReply reply;
                    status = Mem::InvokeTypedSync(
                        &client, static_cast<Mem::Opcode>(TestkitOpcode::Add), request, &reply,
                        priority);
                } else {
                    const uint64_t maxSleepMs =
                        static_cast<uint64_t>(std::max(1, config.maxSleepMs));
                    const uint32_t delayMs = static_cast<uint32_t>(rng() % maxSleepMs);
                    SleepRequest request{delayMs};
                    SleepReply reply;
                    status = Mem::InvokeTypedSync(
                        &client, static_cast<Mem::Opcode>(TestkitOpcode::Sleep), request, &reply,
                        priority);
                }

                if (status != Mem::StatusCode::Ok) {
                    state.failCount.fetch_add(1);
                    RecordError(
                        &state, "rpc failed status=" + std::to_string(static_cast<int>(status)));
                    break;
                }

                if (std::chrono::steady_clock::now() >= warmupEnd) {
                    state.okCount.fetch_add(1);
                    state.lastOkMs.store(Mem::MonotonicNowMs64());
                }

                if (!InBurstWindow(elapsedMs, config)) {
                    const uint64_t burstMultiplier =
                        static_cast<uint64_t>(std::max(1, config.burstMultiplier));
                    const int sleepUs =
                        static_cast<int>((rng() % 1000ULL) * burstMultiplier);
                    if (sleepUs > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
                    }
                }
            }
        });
    }

    std::thread monitor([&]() {
        while (!state.stop.load() && std::chrono::steady_clock::now() < end) {
            const uint64_t nowMs = Mem::MonotonicNowMs64();
            const uint64_t lastOk = state.lastOkMs.load();
            if (nowMs > lastOk &&
                (nowMs - lastOk) / 1000 > static_cast<uint64_t>(config.noProgressTimeoutSec)) {
                RecordError(&state, "no progress within timeout");
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    for (auto& worker : workers) {
        worker.join();
    }
    state.stop.store(true);
    monitor.join();

    client.Shutdown();
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);

    if (!state.error.empty()) {
        HILOGE("stress failed: %s", state.error.c_str());
        return false;
    }

    HILOGI("stress ok: ops=%{public}llu",
           static_cast<unsigned long long>(state.okCount.load()));
    return true;
}

}  // namespace

bool RunStressMain(const StressConfig& config) {
    return RunStress(config);
}

}  // namespace VirusExecutorService::testkit

int main() {
    const auto config = VirusExecutorService::testkit::ParseStressConfigFromEnv();
    HILOGI("stress start: duration=%{public}d warmup=%{public}d threads=%{public}d",
           config.durationSec, config.warmupSec, config.threads);
    return VirusExecutorService::testkit::RunStressMain(config) ? 0 : 1;
}
