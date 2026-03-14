#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_service.h"

namespace virus_executor_service::testkit {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles) {
    if (handles.shmFd >= 0) close(handles.shmFd);
    if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0) close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
}

struct PerfConfig {
    int threads = 1;
    int durationMs = 1000;
    int warmupMs = 200;
    std::filesystem::path baselinePath;
};

struct PerfCaseResult {
    std::string key;
    double opsPerSec = 0.0;
    std::string error;
};

enum class RpcKind { Echo, Add, Sleep };

int GetEnvInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : defaultValue;
    } catch (const std::exception&) {
        return defaultValue;
    }
}

std::filesystem::path GetBaselinePath() {
    const char* path = std::getenv("MEMRPC_PERF_BASELINE_PATH");
    if (path != nullptr && *path != '\0') {
        return std::filesystem::path(path);
    }
    return std::filesystem::current_path() / "perf_baselines" / "testkit_throughput.baseline";
}

PerfConfig LoadPerfConfig() {
    const unsigned int hwThreads = std::thread::hardware_concurrency();
    const unsigned int normalizedHw = hwThreads == 0 ? 1u : hwThreads;
    const int defaultThreads = std::max(1, static_cast<int>(std::min(4u, normalizedHw)));
    PerfConfig config;
    config.threads = GetEnvInt("MEMRPC_PERF_THREADS", defaultThreads);
    config.durationMs = GetEnvInt("MEMRPC_PERF_durationMs", 1000);
    config.warmupMs = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);
    config.baselinePath = GetBaselinePath();
    return config;
}

const char* RpcKindName(RpcKind kind) {
    switch (kind) {
        case RpcKind::Echo:
            return "echo";
        case RpcKind::Add:
            return "add";
        case RpcKind::Sleep:
            return "sleep";
    }
    return "unknown";
}

std::string MakeBaselineKey(RpcKind kind, int threads) {
    std::ostringstream stream;
    stream << RpcKindName(kind) << ".threads=" << threads;
    return stream.str();
}

bool InvokeOnce(TestkitClient* client,
                RpcKind kind,
                const std::string& echoText,
                EchoReply* echoReply,
                AddReply* addReply,
                SleepReply* sleepReply,
                MemRpc::StatusCode* status) {
    if (client == nullptr) {
        if (status != nullptr) {
            *status = MemRpc::StatusCode::InvalidArgument;
        }
        return false;
    }

    MemRpc::StatusCode callStatus = MemRpc::StatusCode::InvalidArgument;
    switch (kind) {
        case RpcKind::Echo:
            callStatus = client->Echo(echoText, echoReply);
            break;
        case RpcKind::Add:
            callStatus = client->Add(1, 2, addReply);
            break;
        case RpcKind::Sleep:
            callStatus = client->Sleep(0, sleepReply);
            break;
    }
    if (status != nullptr) {
        *status = callStatus;
    }
    return callStatus == MemRpc::StatusCode::Ok;
}

struct WorkerResult {
    uint64_t ops = 0;
    bool ok = true;
    MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
};

bool MeasureOpsPerSecond(const PerfConfig& config,
                         RpcKind kind,
                         const std::shared_ptr<MemRpc::IBootstrapChannel>& bootstrap,
                         double* opsPerSec,
                         std::string* error) {
    if (opsPerSec == nullptr) {
        if (error != nullptr) {
            *error = "missing opsPerSec output";
        }
        return false;
    }

    const int threadCount = std::max(1, config.threads);
    TestkitClient client(bootstrap);
    const auto initStatus = client.Init();
    if (initStatus != MemRpc::StatusCode::Ok) {
        if (error != nullptr) {
            std::ostringstream stream;
            stream << "client init failed status=" << static_cast<int>(initStatus);
            *error = stream.str();
        }
        return false;
    }

    std::vector<WorkerResult> workerResults(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    const auto startTime =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    const auto warmupEnd = startTime + std::chrono::milliseconds(config.warmupMs);
    const auto endTime = warmupEnd + std::chrono::milliseconds(config.durationMs);

    for (int i = 0; i < threadCount; ++i) {
        workers.emplace_back([&, i]() {
            WorkerResult& result = workerResults[i];
            while (std::chrono::steady_clock::now() < startTime) {
                std::this_thread::yield();
            }

            EchoReply echoReply;
            AddReply addReply;
            SleepReply sleepReply;
            const std::string echoText = "ping";

            while (std::chrono::steady_clock::now() < warmupEnd) {
                if (!InvokeOnce(
                        &client, kind, echoText, &echoReply, &addReply, &sleepReply,
                        &result.status)) {
                    result.ok = false;
                    break;
                }
            }

            while (result.ok && std::chrono::steady_clock::now() < endTime) {
                if (!InvokeOnce(
                        &client, kind, echoText, &echoReply, &addReply, &sleepReply,
                        &result.status)) {
                    result.ok = false;
                    break;
                }
                ++result.ops;
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    client.Shutdown();

    for (const auto& result : workerResults) {
        if (!result.ok) {
            if (error != nullptr) {
                std::ostringstream stream;
                stream << "rpc failed status=" << static_cast<int>(result.status);
                *error = stream.str();
            }
            return false;
        }
    }

    const double durationSeconds = static_cast<double>(std::max(1, config.durationMs)) / 1000.0;
    uint64_t totalOps = 0;
    for (const auto& result : workerResults) {
        totalOps += result.ops;
    }
    *opsPerSec = totalOps / durationSeconds;
    return true;
}

std::vector<PerfCaseResult> RunThroughputSuite(const PerfConfig& config) {
    std::vector<PerfCaseResult> results;
    const int threadCount = std::max(1, config.threads);
    const std::vector<RpcKind> kinds = {RpcKind::Echo, RpcKind::Add, RpcKind::Sleep};

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles{};
    if (bootstrap->OpenSession(handles) != MemRpc::StatusCode::Ok) {
        for (const auto kind : kinds) {
            results.push_back({MakeBaselineKey(kind, threadCount), 0.0, "bootstrap start failed"});
        }
        return results;
    }
    CloseHandles(handles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    MemRpc::ServerOptions options;
    options.highWorkerThreads = static_cast<uint32_t>(threadCount);
    options.normalWorkerThreads = static_cast<uint32_t>(threadCount);
    server.SetOptions(options);
    TestkitService service;
    service.RegisterHandlers(&server);
    if (server.Start() != MemRpc::StatusCode::Ok) {
        for (const auto kind : kinds) {
            results.push_back({MakeBaselineKey(kind, threadCount), 0.0, "server start failed"});
        }
        return results;
    }

    for (const auto kind : kinds) {
        PerfCaseResult result;
        result.key = MakeBaselineKey(kind, threadCount);
        if (!MeasureOpsPerSecond(config, kind, bootstrap, &result.opsPerSec, &result.error) &&
            result.error.empty()) {
            result.error = "measurement failed";
        }
        results.push_back(result);
    }

    server.Stop();
    return results;
}

std::map<std::string, double> LoadBaseline(const std::filesystem::path& path) {
    std::map<std::string, double> baseline;
    std::ifstream input(path);
    if (!input.is_open()) {
        return baseline;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            baseline[key] = std::stod(value);
        } catch (const std::exception&) {
            continue;
        }
    }
    return baseline;
}

bool WriteBaseline(const std::filesystem::path& path,
                   const std::map<std::string, double>& baseline) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << "# testkit throughput baseline\n";
    for (const auto& entry : baseline) {
        output << entry.first << "=" << std::fixed << std::setprecision(3) << entry.second
               << "\n";
    }
    return true;
}

bool UpdateBaseline(const std::filesystem::path& path,
                    const std::vector<PerfCaseResult>& results,
                    std::map<std::string, double>* baseline) {
    if (baseline == nullptr) {
        return false;
    }
    bool updated = false;
    for (const auto& result : results) {
        if (!result.error.empty()) {
            ADD_FAILURE() << "Throughput measurement failed for " << result.key << ": "
                          << result.error;
            continue;
        }
        auto it = baseline->find(result.key);
        if (it == baseline->end()) {
            (*baseline)[result.key] = result.opsPerSec;
            updated = true;
            continue;
        }

        const double baselineValue = it->second;
        if (baselineValue > 0.0 && result.opsPerSec < baselineValue * 0.9) {
            ADD_FAILURE() << "Throughput regression for " << result.key << ": baseline="
                          << baselineValue << " current=" << result.opsPerSec;
            continue;
        }
        if (result.opsPerSec > baselineValue) {
            it->second = result.opsPerSec;
            updated = true;
        }
    }

    if (updated) {
        WriteBaseline(path, *baseline);
    }
    return updated;
}

}  // namespace

TEST(TestkitThroughputTest, RecordsAndValidatesBaseline) {
    const PerfConfig config = LoadPerfConfig();
    const std::vector<PerfCaseResult> results = RunThroughputSuite(config);
    std::map<std::string, double> baseline = LoadBaseline(config.baselinePath);
    const bool updated = UpdateBaseline(config.baselinePath, results, &baseline);

    EXPECT_FALSE(results.empty());
    EXPECT_TRUE(updated || !baseline.empty() || std::filesystem::exists(config.baselinePath));
}

}  // namespace virus_executor_service::testkit
