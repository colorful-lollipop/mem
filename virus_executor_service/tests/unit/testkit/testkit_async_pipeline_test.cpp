#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_async_client.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_service.h"

namespace virus_executor_service::testkit {
namespace {

void CloseHandles(memrpc::BootstrapHandles& handles) {
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

struct PipelineResult {
    int batchSize = 0;
    double opsPerSec = 0;
    uint64_t totalOps = 0;
};

}  // namespace

TEST(TestkitAsyncPipelineTest, BatchSizeThroughput) {
    const int durationMs = GetEnvInt("MEMRPC_PERF_durationMs", 1000);
    const int warmupMs = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);

    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);

    memrpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    memrpc::ServerOptions options;
    options.highWorkerThreads = 4;
    options.normalWorkerThreads = 4;
    server.SetOptions(options);
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    double syncOpsPerSec = 0;
    {
        TestkitClient syncClient(bootstrap);
        ASSERT_EQ(syncClient.Init(), memrpc::StatusCode::Ok);

        const auto warmupEnd =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
        while (std::chrono::steady_clock::now() < warmupEnd) {
            EchoReply reply;
            syncClient.Echo("ping", &reply);
        }

        uint64_t syncOps = 0;
        const auto endTime =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
        while (std::chrono::steady_clock::now() < endTime) {
            EchoReply reply;
            const auto status = syncClient.Echo("ping", &reply);
            ASSERT_EQ(status, memrpc::StatusCode::Ok);
            ++syncOps;
        }
        const double durationSec = std::max(1, durationMs) / 1000.0;
        syncOpsPerSec = syncOps / durationSec;
        syncClient.Shutdown();
    }

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), memrpc::StatusCode::Ok);

    const std::vector<int> batchSizes = {1, 8, 32, 64};
    std::vector<PipelineResult> results;

    std::cout << "\n=== Testkit Async Pipeline Throughput ===" << std::endl;

    for (const int batchSize : batchSizes) {
        const auto warmupEnd =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
        while (std::chrono::steady_clock::now() < warmupEnd) {
            EchoRequest request;
            request.text = "ping";
            std::vector<memrpc::TypedFuture<EchoReply>> futures;
            futures.reserve(batchSize);
            for (int i = 0; i < batchSize; ++i) {
                futures.push_back(asyncClient.EchoAsync(request));
            }
            for (auto& future : futures) {
                EchoReply reply;
                future.Wait(&reply);
            }
        }

        uint64_t totalOps = 0;
        const auto endTime =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
        while (std::chrono::steady_clock::now() < endTime) {
            EchoRequest request;
            request.text = "ping";
            std::vector<memrpc::TypedFuture<EchoReply>> futures;
            futures.reserve(batchSize);
            for (int i = 0; i < batchSize; ++i) {
                futures.push_back(asyncClient.EchoAsync(request));
            }
            bool allOk = true;
            for (auto& future : futures) {
                EchoReply reply;
                const auto status = future.Wait(&reply);
                if (status != memrpc::StatusCode::Ok) {
                    allOk = false;
                }
            }
            if (!allOk) {
                ADD_FAILURE() << "async batch failed for batchSize=" << batchSize;
                break;
            }
            totalOps += batchSize;
        }

        const double durationSec = std::max(1, durationMs) / 1000.0;
        PipelineResult result;
        result.batchSize = batchSize;
        result.totalOps = totalOps;
        result.opsPerSec = totalOps / durationSec;
        results.push_back(result);

        std::cout << "  batch=" << std::setw(3) << batchSize << ": " << std::fixed
                  << std::setprecision(0) << result.opsPerSec << " ops/sec" << std::endl;
    }

    std::cout << "  sync:     " << std::fixed << std::setprecision(0) << syncOpsPerSec
              << " ops/sec" << std::endl;

    asyncClient.Shutdown();
    server.Stop();

    EXPECT_FALSE(results.empty());
    if (results.size() >= 2) {
        EXPECT_GT(results.back().opsPerSec, results.front().opsPerSec * 0.8);
    }
}

}  // namespace virus_executor_service::testkit
