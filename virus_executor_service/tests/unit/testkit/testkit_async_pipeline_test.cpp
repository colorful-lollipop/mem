#include <gtest/gtest.h>

#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_async_client.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_service.h"

namespace VirusExecutorService::testkit {
namespace {

bool ThreadSanitizerEnabled()
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#endif
#endif
#if defined(__SANITIZE_THREAD__)
    return true;
#endif
    return false;
}

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

struct PipelineResult {
    int batchSize = 0;
    double opsPerSec = 0;
    uint64_t totalOps = 0;
};

}  // namespace

TEST(TestkitAsyncPipelineTest, BatchSizeThroughput)
{
    const int defaultDurationMs = ThreadSanitizerEnabled() ? 250 : 500;
    const int defaultWarmupMs = ThreadSanitizerEnabled() ? 50 : 100;
    const int durationMs = GetEnvInt("MEMRPC_PERF_durationMs", defaultDurationMs);
    const int warmupMs = GetEnvInt("MEMRPC_PERF_WARMUP_MS", defaultWarmupMs);

    const MemRpc::SharedMemorySessionConfig bootstrapConfig;
    double syncOpsPerSec = 0;
    {
        auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(bootstrapConfig);
        MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
        CloseHandles(handles);

        MemRpc::RpcServer server;
        server.SetBootstrapHandles(bootstrap->serverHandles());
        MemRpc::ServerOptions options;
        options.highWorkerThreads = 4;
        options.normalWorkerThreads = 4;
        server.SetOptions(options);
        TestkitService service;
        RegisterHandlersToServer(&service, &server);
        ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

        TestkitClient syncClient(bootstrap);
        ASSERT_EQ(syncClient.Init(), MemRpc::StatusCode::Ok);

        const auto warmupEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
        while (std::chrono::steady_clock::now() < warmupEnd) {
            EchoReply reply;
            syncClient.Echo("ping", &reply);
        }

        uint64_t syncOps = 0;
        const auto endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
        while (std::chrono::steady_clock::now() < endTime) {
            EchoReply reply;
            const auto status = syncClient.Echo("ping", &reply);
            ASSERT_EQ(status, MemRpc::StatusCode::Ok);
            ++syncOps;
        }
        const double durationSec = std::max(1, durationMs) / 1000.0;
        syncOpsPerSec = static_cast<double>(syncOps) / durationSec;
        syncClient.Shutdown();
        server.Stop();
    }

    const int requestRingSize = static_cast<int>(bootstrapConfig.normalRingSize);
    const std::vector<int> batchSizes = {
        1,
        std::max(2, requestRingSize / 2),
        std::max(2, requestRingSize),
    };
    std::vector<PipelineResult> results;

    std::cout << "\n=== Testkit Async Pipeline Throughput ===" << std::endl;

    {
        auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(bootstrapConfig);
        MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
        CloseHandles(handles);

        MemRpc::RpcServer server;
        server.SetBootstrapHandles(bootstrap->serverHandles());
        MemRpc::ServerOptions options;
        options.highWorkerThreads = 4;
        options.normalWorkerThreads = 4;
        server.SetOptions(options);
        TestkitService service;
        RegisterHandlersToServer(&service, &server);
        ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

        TestkitAsyncClient asyncClient(bootstrap);
        ASSERT_EQ(asyncClient.Init(), MemRpc::StatusCode::Ok);

        for (const int batchSize : batchSizes) {
            const auto warmupEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
            while (std::chrono::steady_clock::now() < warmupEnd) {
                EchoRequest request;
                request.text = "ping";
                std::vector<MemRpc::TypedFuture<EchoReply>> futures;
                futures.reserve(static_cast<std::size_t>(batchSize));
                for (int i = 0; i < batchSize; ++i) {
                    futures.push_back(asyncClient.EchoAsync(request));
                }
                for (auto& future : futures) {
                    EchoReply reply;
                    std::move(future).Wait(&reply);
                }
            }

            uint64_t totalOps = 0;
            const auto endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
            while (std::chrono::steady_clock::now() < endTime) {
                EchoRequest request;
                request.text = "ping";
                std::vector<MemRpc::TypedFuture<EchoReply>> futures;
                futures.reserve(static_cast<std::size_t>(batchSize));
                for (int i = 0; i < batchSize; ++i) {
                    futures.push_back(asyncClient.EchoAsync(request));
                }
                bool allOk = true;
                std::vector<int> failedStatuses;
                for (auto& future : futures) {
                    EchoReply reply;
                    const auto status = std::move(future).Wait(&reply);
                    if (status != MemRpc::StatusCode::Ok) {
                        allOk = false;
                        failedStatuses.push_back(static_cast<int>(status));
                    }
                }
                if (!allOk) {
                    std::ostringstream failure;
                    failure << "async batch failed for batchSize=" << batchSize << " statuses=";
                    for (size_t idx = 0; idx < failedStatuses.size(); ++idx) {
                        if (idx != 0) {
                            failure << ',';
                        }
                        failure << failedStatuses[idx];
                    }
                    ADD_FAILURE() << failure.str();
                    break;
                }
                totalOps += static_cast<uint64_t>(batchSize);
            }

            const double durationSec = std::max(1, durationMs) / 1000.0;
            PipelineResult result;
            result.batchSize = batchSize;
            result.totalOps = totalOps;
            result.opsPerSec = static_cast<double>(totalOps) / durationSec;
            results.push_back(result);

            std::cout << "  batch=" << std::setw(3) << batchSize << ": " << std::fixed << std::setprecision(0)
                      << result.opsPerSec << " ops/sec" << std::endl;
        }

        asyncClient.Shutdown();
        server.Stop();
    }

    std::cout << "  sync:     " << std::fixed << std::setprecision(0) << syncOpsPerSec << " ops/sec" << std::endl;

    EXPECT_FALSE(results.empty());
    if (results.size() >= 2) {
        EXPECT_GT(results.back().opsPerSec, results.front().opsPerSec * 0.8);
    }
}

}  // namespace VirusExecutorService::testkit
