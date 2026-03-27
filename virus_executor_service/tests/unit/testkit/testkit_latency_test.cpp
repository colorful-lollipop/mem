#include <gtest/gtest.h>

#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/core/protocol.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_service.h"

namespace VirusExecutorService::testkit {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles)
{
    if (handles.shmFd >= 0)
        close(handles.shmFd);
    if (handles.highReqEventFd >= 0)
        close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0)
        close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0)
        close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0)
        close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0)
        close(handles.respCreditEventFd);
}

int GetEnvInt(const char* name, int defaultValue)
{
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

struct LatencyStats {
    double p50Us = 0;
    double p99Us = 0;
    double p999Us = 0;
    double maxUs = 0;
    double meanUs = 0;
    uint64_t samples = 0;
};

LatencyStats ComputeStats(std::vector<double>& latenciesUs)
{
    LatencyStats stats;
    if (latenciesUs.empty()) {
        return stats;
    }
    std::sort(latenciesUs.begin(), latenciesUs.end());
    const std::size_t sampleCount = latenciesUs.size();
    stats.samples = static_cast<uint64_t>(sampleCount);
    stats.meanUs = std::accumulate(latenciesUs.begin(), latenciesUs.end(), 0.0) / static_cast<double>(sampleCount);
    stats.p50Us = latenciesUs[sampleCount * 50 / 100];
    stats.p99Us = latenciesUs[sampleCount * 99 / 100];
    stats.p999Us = latenciesUs[std::min(sampleCount - 1, sampleCount * 999 / 1000)];
    stats.maxUs = latenciesUs.back();
    return stats;
}

void PrintStats(const char* label, const LatencyStats& stats)
{
    std::cout << std::setw(20) << label << ": "
              << "p50=" << std::fixed << std::setprecision(1) << stats.p50Us << "us  "
              << "p99=" << stats.p99Us << "us  "
              << "p999=" << stats.p999Us << "us  "
              << "max=" << stats.maxUs << "us  "
              << "mean=" << stats.meanUs << "us  "
              << "n=" << stats.samples << std::endl;
}

std::size_t MaxEchoTextBytes()
{
    const std::size_t inlineLimit =
        std::min<std::size_t>(MemRpc::DEFAULT_MAX_REQUEST_BYTES, MemRpc::DEFAULT_MAX_RESPONSE_BYTES);
    return inlineLimit > sizeof(uint32_t) ? inlineLimit - sizeof(uint32_t) : 0;
}

}  // namespace

TEST(TestkitLatencyTest, SingleThreadSerialLatencyByPayloadSize)
{
    const int iterations = GetEnvInt("MEMRPC_LATENCY_ITERATIONS", 2000);
    const int warmup = GetEnvInt("MEMRPC_LATENCY_WARMUP", 200);

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    RegisterHandlersToServer(&service, &server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    struct PayloadCase {
        const char* name;
        std::string text;
    };
    const std::size_t maxEchoTextBytes = MaxEchoTextBytes();
    const std::vector<PayloadCase> cases = {
        {"0B", ""},
        {"64B", std::string(64, 'x')},
        {"256B", std::string(std::min<std::size_t>(256, maxEchoTextBytes), 'x')},
        {"max-inline", std::string(maxEchoTextBytes, 'x')},
    };

    std::cout << "\n=== Testkit RPC Latency Distribution ===" << std::endl;

    for (const auto& payloadCase : cases) {
        for (int i = 0; i < warmup; ++i) {
            EchoReply reply;
            ASSERT_EQ(client.Echo(payloadCase.text, &reply), MemRpc::StatusCode::Ok);
        }

        std::vector<double> latencies;
        latencies.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            EchoReply reply;
            const auto start = std::chrono::steady_clock::now();
            const auto status = client.Echo(payloadCase.text, &reply);
            const auto end = std::chrono::steady_clock::now();
            ASSERT_EQ(status, MemRpc::StatusCode::Ok);
            latencies.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
                1000.0);
        }
        const LatencyStats stats = ComputeStats(latencies);
        PrintStats(payloadCase.name, stats);
        EXPECT_GT(stats.samples, 0u);
    }

    client.Shutdown();
    server.Stop();
}

TEST(TestkitLatencyTest, DirectCallBaselineLatency)
{
    const int iterations = GetEnvInt("MEMRPC_LATENCY_ITERATIONS", 2000);
    TestkitService service;

    std::cout << "\n=== Testkit Direct Call Latency ===" << std::endl;

    const std::vector<std::pair<const char*, std::string>> cases = {
        {"0B (direct)", ""},
        {"64B (direct)", std::string(64, 'x')},
        {"256B (direct)", std::string(std::min<std::size_t>(256, MaxEchoTextBytes()), 'x')},
        {"max-inline (direct)", std::string(MaxEchoTextBytes(), 'x')},
    };

    for (const auto& payloadCase : cases) {
        std::vector<double> latencies;
        latencies.reserve(static_cast<std::size_t>(iterations));
        EchoRequest request;
        request.text = payloadCase.second;
        for (int i = 0; i < iterations; ++i) {
            const auto start = std::chrono::steady_clock::now();
            EchoReply reply = service.Echo(request);
            const auto end = std::chrono::steady_clock::now();
            (void)reply;
            latencies.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
                1000.0);
        }
        const LatencyStats stats = ComputeStats(latencies);
        PrintStats(payloadCase.first, stats);
        EXPECT_GT(stats.samples, 0u);
    }
}

}  // namespace VirusExecutorService::testkit
