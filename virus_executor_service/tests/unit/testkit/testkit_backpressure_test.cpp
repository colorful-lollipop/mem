#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
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

bool WaitForCondition(const std::function<bool()>& condition, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

}  // namespace

TEST(TestkitBackpressureTest, SlotExhaustionAndRecovery) {
    MemRpc::DevBootstrapConfig config;
    config.slotCount = 4;
    config.highRingSize = 8;
    config.normalRingSize = 8;
    config.responseRingSize = 8;
    config.maxRequestBytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES;
    config.maxResponseBytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES;

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.SetOptions({.highWorkerThreads = 4, .normalWorkerThreads = 4});
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    std::vector<std::thread> callers;
    std::atomic<int> completed{0};
    for (int i = 0; i < 4; ++i) {
        callers.emplace_back([&client, &completed]() {
            SleepReply reply;
            const auto status = client.Sleep(500, &reply);
            if (status == MemRpc::StatusCode::Ok) {
                completed.fetch_add(1);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EchoReply reply;
    const auto status = client.Echo("under_pressure", &reply);
    EXPECT_EQ(status, MemRpc::StatusCode::Ok);
    if (status == MemRpc::StatusCode::Ok) {
        EXPECT_EQ(reply.text, "under_pressure");
    }

    for (auto& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(completed.load(), 4);

    EchoReply recoveredReply;
    EXPECT_EQ(client.Echo("recovered", &recoveredReply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(recoveredReply.text, "recovered");

    client.Shutdown();
    server.Stop();
}

TEST(TestkitBackpressureTest, CreditFlowReleasesBlockedSubmitter) {
    MemRpc::DevBootstrapConfig config;
    config.slotCount = 2;
    config.highRingSize = 4;
    config.normalRingSize = 4;
    config.responseRingSize = 4;
    config.maxRequestBytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES;
    config.maxResponseBytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES;

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.SetOptions({.highWorkerThreads = 2, .normalWorkerThreads = 2});
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    std::vector<std::thread> callers;
    std::vector<MemRpc::StatusCode> results(2, MemRpc::StatusCode::InvalidArgument);
    for (int i = 0; i < 2; ++i) {
        callers.emplace_back([&client, &results, i]() {
            SleepReply reply;
            results[i] = client.Sleep(300, &reply);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::atomic<bool> thirdDone{false};
    MemRpc::StatusCode thirdStatus = MemRpc::StatusCode::InvalidArgument;
    std::thread thirdThread([&]() {
        EchoReply reply;
        thirdStatus = client.Echo("third", &reply);
        thirdDone.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto& caller : callers) {
        caller.join();
    }
    for (const auto result : results) {
        EXPECT_EQ(result, MemRpc::StatusCode::Ok);
    }

    ASSERT_TRUE(WaitForCondition([&thirdDone] { return thirdDone.load(); }, 2000));
    thirdThread.join();
    EXPECT_EQ(thirdStatus, MemRpc::StatusCode::Ok);

    client.Shutdown();
    server.Stop();
}

}  // namespace VirusExecutorService::testkit
