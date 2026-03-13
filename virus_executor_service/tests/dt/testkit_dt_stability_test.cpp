#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"
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

uint32_t GetThreadCount() {
    const unsigned int hw = std::thread::hardware_concurrency();
    const unsigned int hwThreads = hw == 0 ? 1u : hw;
    const int defaultThreads = std::max(1, static_cast<int>(std::min(4u, hwThreads)));
    return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", defaultThreads));
}

}  // namespace

TEST(TestkitDtStabilityTest, ShortRandomLoadStaysHealthy) {
    const int durationMs = GetEnvInt("MEMRPC_DT_durationMs", 3000);
    const int progressTimeoutMs = GetEnvInt("MEMRPC_DT_progressTimeoutMs", 200);
    const uint32_t threadCount = GetThreadCount();

    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);

    memrpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    memrpc::ServerOptions options;
    options.highWorkerThreads = threadCount;
    options.normalWorkerThreads = threadCount;
    server.SetOptions(options);
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    std::atomic<uint64_t> success{0};
    std::atomic<memrpc::StatusCode> firstError{memrpc::StatusCode::Ok};
    std::atomic<int64_t> lastSuccessMs{0};

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(durationMs);

    auto nowMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    lastSuccessMs.store(nowMs());

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937 rng(static_cast<uint32_t>(i + 7));
            while (std::chrono::steady_clock::now() < deadline) {
                const int choice = static_cast<int>(rng() % 100);
                memrpc::StatusCode status = memrpc::StatusCode::Ok;
                if (choice < 70) {
                    EchoReply reply;
                    status = client.Echo("ping", &reply);
                } else if (choice < 95) {
                    AddReply reply;
                    status = client.Add(1, 2, &reply);
                } else {
                    SleepReply reply;
                    status = client.Sleep(1, &reply);
                }
                if (status != memrpc::StatusCode::Ok) {
                    firstError.store(status);
                    return;
                }
                ++success;
                lastSuccessMs.store(nowMs());
            }
        });
    }

    std::atomic<bool> progressOk{true};
    std::thread watchdog([&]() {
        while (std::chrono::steady_clock::now() < deadline) {
            const int64_t last = lastSuccessMs.load();
            if (nowMs() - last > progressTimeoutMs) {
                progressOk.store(false);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    for (auto& worker : workers) {
        worker.join();
    }
    watchdog.join();

    client.Shutdown();
    server.Stop();

    EXPECT_EQ(firstError.load(), memrpc::StatusCode::Ok);
    EXPECT_TRUE(progressOk.load());
    EXPECT_GT(success.load(), 0u);
}

}  // namespace virus_executor_service::testkit
