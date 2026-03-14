#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/core/protocol.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_service.h"

namespace VirusExecutorService::testkit {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles) {
    if (handles.shmFd >= 0) close(handles.shmFd);
    if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0) close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
}

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

uint32_t GetThreadCount() {
    const unsigned int hw = std::thread::hardware_concurrency();
    const unsigned int hwThreads = hw == 0 ? 1u : hw;
    const int defaultThreads = std::max(1, static_cast<int>(std::min(4u, hwThreads)));
    return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", defaultThreads));
}

std::filesystem::path BaselinePath() {
    const char* dir = std::getenv("MEMRPC_DT_BASELINE_DIR");
    std::filesystem::path base =
        (dir && *dir) ? dir : (std::filesystem::current_path() / "perf_baselines");
    return base / "testkit_dt_perf.baseline";
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
    output << "# testkit dt perf baseline\n";
    for (const auto& entry : baseline) {
        output << entry.first << "=" << std::fixed << std::setprecision(3) << entry.second
               << "\n";
    }
    return true;
}

struct PerfStats {
    double opsPerSec = 0.0;
    double p99Us = 0.0;
};

PerfStats ComputeStats(const std::vector<double>& latenciesUs,
                       double durationSec,
                       uint64_t ops) {
    PerfStats stats;
    stats.opsPerSec = durationSec > 0.0 ? ops / durationSec : 0.0;
    if (!latenciesUs.empty()) {
        std::vector<double> sorted = latenciesUs;
        std::sort(sorted.begin(), sorted.end());
        stats.p99Us = sorted[sorted.size() * 99 / 100];
    }
    return stats;
}

std::size_t MaxEchoTextBytes() {
    const std::size_t inlineLimit = std::min<std::size_t>(
        MemRpc::DEFAULT_MAX_REQUEST_BYTES, MemRpc::DEFAULT_MAX_RESPONSE_BYTES);
    return inlineLimit > sizeof(uint32_t) ? inlineLimit - sizeof(uint32_t) : 0;
}

}  // namespace

TEST(TestkitDtPerfTest, ShortPerfBaseline) {
    const int durationMs = GetEnvInt("MEMRPC_DT_durationMs", 3000);
    const int warmupMs = GetEnvInt("MEMRPC_DT_WARMUP_MS", 200);
    const int minOps = GetEnvInt("MEMRPC_DT_MIN_OPS", 50);
    const int maxP99Us = GetEnvInt("MEMRPC_DT_MAX_P99_US", 20000);
    const uint32_t threadCount = GetThreadCount();

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    MemRpc::ServerOptions options;
    options.highWorkerThreads = threadCount;
    options.normalWorkerThreads = threadCount;
    server.SetOptions(options);
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    const std::vector<std::pair<const char*, size_t>> cases = {
        {"echo_0B", 0},
        {"echo_max_inline", MaxEchoTextBytes()},
        {"add", 0},
    };

    std::map<std::string, double> baseline = LoadBaseline(BaselinePath());
    bool baselineUpdated = false;

    for (const auto& current : cases) {
        const std::string caseName = current.first;
        const size_t payloadSize = current.second;

        const auto warmupEnd =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
        while (std::chrono::steady_clock::now() < warmupEnd) {
            if (caseName == "add") {
                AddReply reply;
                (void)client.Add(1, 2, &reply);
            } else {
                EchoReply reply;
                std::string payload(payloadSize, 'x');
                (void)client.Echo(payload, &reply);
            }
        }

        std::atomic<uint64_t> ops{0};
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(durationMs);
        for (uint32_t i = 0; i < threadCount; ++i) {
            workers.emplace_back([&, i]() {
                while (std::chrono::steady_clock::now() < end) {
                    MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
                    if (caseName == "add") {
                        AddReply reply;
                        status = client.Add(1, 2, &reply);
                    } else {
                        EchoReply reply;
                        std::string payload(payloadSize, static_cast<char>('a' + (i % 8)));
                        status = client.Echo(payload, &reply);
                    }
                    if (status == MemRpc::StatusCode::Ok) {
                        ++ops;
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        std::vector<double> latencies;
        latencies.reserve(500);
        for (int i = 0; i < 500; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
            if (caseName == "add") {
                AddReply reply;
                status = client.Add(1, 2, &reply);
            } else {
                EchoReply reply;
                std::string payload(payloadSize, 'x');
                status = client.Echo(payload, &reply);
            }
            const auto t1 = std::chrono::steady_clock::now();
            ASSERT_EQ(status, MemRpc::StatusCode::Ok);
            latencies.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
                1000.0);
        }

        const double durationSec = std::max(1, durationMs) / 1000.0;
        const PerfStats stats = ComputeStats(latencies, durationSec, ops.load());

        const std::string opsKey =
            "testkit." + caseName + ".threads=" + std::to_string(threadCount) + ".ops_per_sec";
        const std::string p99Key =
            "testkit." + caseName + ".threads=" + std::to_string(threadCount) + ".p99_us";

        EXPECT_GE(stats.opsPerSec, static_cast<double>(minOps));
        EXPECT_LE(stats.p99Us, static_cast<double>(maxP99Us));

        const double opsBaseline = baseline.count(opsKey) ? baseline[opsKey] : 0.0;
        const double p99Baseline = baseline.count(p99Key) ? baseline[p99Key] : 0.0;

        if (opsBaseline > 0.0 && stats.opsPerSec < opsBaseline * 0.9) {
            ADD_FAILURE() << "Throughput regression for " << opsKey << ": baseline="
                          << opsBaseline << " current=" << stats.opsPerSec;
        }
        if (p99Baseline > 0.0 && stats.p99Us > p99Baseline * 1.1) {
            ADD_FAILURE() << "Latency regression for " << p99Key << ": baseline="
                          << p99Baseline << " current=" << stats.p99Us;
        }

        if (stats.opsPerSec > opsBaseline) {
            baseline[opsKey] = stats.opsPerSec;
            baselineUpdated = true;
        }
        if (p99Baseline <= 0.0 || stats.p99Us < p99Baseline) {
            baseline[p99Key] = stats.p99Us;
            baselineUpdated = true;
        }
    }

    if (baselineUpdated) {
        WriteBaseline(BaselinePath(), baseline);
    }

    client.Shutdown();
    server.Stop();
}

}  // namespace VirusExecutorService::testkit
