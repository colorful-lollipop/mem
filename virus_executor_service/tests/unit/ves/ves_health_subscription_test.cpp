#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include "client/ves_client.h"
#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "service/virus_executor_service.h"
#include "system_ability.h"
#include "transport/ves_control_stub.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

namespace {

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void CloseHandles(MemRpc::BootstrapHandles& handles)
{
    if (handles.shmFd >= 0) {
        close(handles.shmFd);
    }
    if (handles.highReqEventFd >= 0) {
        close(handles.highReqEventFd);
    }
    if (handles.normalReqEventFd >= 0) {
        close(handles.normalReqEventFd);
    }
    if (handles.respEventFd >= 0) {
        close(handles.respEventFd);
    }
    if (handles.reqCreditEventFd >= 0) {
        close(handles.reqCreditEventFd);
    }
    if (handles.respCreditEventFd >= 0) {
        close(handles.respCreditEventFd);
    }
}

class FakeSubscriptionControlService final : public OHOS::SystemAbility,
                                             public VesControlStub {
 public:
    FakeSubscriptionControlService()
        : OHOS::SystemAbility(VES_CONTROL_SA_ID, true),
          bootstrap_(std::make_shared<MemRpc::DevBootstrapChannel>()) {}

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        openCount_.fetch_add(1);
        sessionOpen_.store(true);
        return bootstrap_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        closeCount_.fetch_add(1);
        sessionOpen_.store(false);
        return bootstrap_->CloseSession();
    }

    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override
    {
        reply = {};
        reply.version = 2;
        reply.sessionId = sessionOpen_.load() ? bootstrap_->serverHandles().sessionId : 0;
        std::snprintf(reply.currentTask, sizeof(reply.currentTask), "%s", "idle");
        if (sessionOpen_.load() && healthy_.load()) {
            reply.status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
            reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::None);
            reply.flags = VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED;
        } else {
            reply.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession);
            reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession);
        }
        return MemRpc::StatusCode::Ok;
    }

    void OnStart() override {}

    void OnStop() override
    {
        sessionOpen_.store(false);
        (void)bootstrap_->CloseSession();
    }

    void SetServicePathForTest(const std::string& socketPath)
    {
        AsObject()->SetServicePath(socketPath);
    }

    void PrepareServerHandlesForTest()
    {
        MemRpc::BootstrapHandles handles{};
        ASSERT_EQ(bootstrap_->OpenSession(handles), MemRpc::StatusCode::Ok);
        CloseHandles(handles);
    }

    [[nodiscard]] MemRpc::BootstrapHandles serverHandles() const
    {
        return bootstrap_->serverHandles();
    }

    void SetHealthy(bool healthy)
    {
        healthy_.store(healthy);
    }

    [[nodiscard]] int openCount() const
    {
        return openCount_.load();
    }

    [[nodiscard]] int closeCount() const
    {
        return closeCount_.load();
    }

 private:
    std::shared_ptr<MemRpc::DevBootstrapChannel> bootstrap_;
    std::atomic<bool> healthy_{true};
    std::atomic<bool> sessionOpen_{false};
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
};

}  // namespace

TEST(VesHealthSubscriptionTest, DeliversSnapshotsWhenSubscribed) {
    const std::string socketPath =
        "/tmp/ves_health_subscription_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClient client(remote);
    std::atomic<int> snapshotCount{0};
    std::atomic<uint64_t> latestSessionId{0};
    client.SetHealthSnapshotCallback([&](const VesHeartbeatReply& reply) {
        snapshotCount.fetch_add(1);
        latestSessionId.store(reply.sessionId);
    });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ASSERT_TRUE(WaitFor([&]() { return snapshotCount.load() > 0; },
                        std::chrono::milliseconds(500)));
    EXPECT_NE(latestSessionId.load(), 0u);

    client.Shutdown();
    service->OnStop();
}

TEST(VesHealthSubscriptionTest, BlockingSubscriberDoesNotBlockRecovery) {
    const std::string socketPath =
        "/tmp/ves_health_subscription_blocking_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<FakeSubscriptionControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest&) {
            ScanFileReply reply;
            reply.code = 0;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClient client(remote);
    client.SetHealthSnapshotCallback([](const VesHeartbeatReply&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        throw std::runtime_error("side-channel failure");
    });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile("/data/subscription.bin", &reply), MemRpc::StatusCode::Ok);

    const int initialOpenCount = service->openCount();
    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; },
                        std::chrono::milliseconds(250)));
    service->SetHealthy(true);
    ASSERT_TRUE(WaitFor([&]() { return service->openCount() > initialOpenCount; },
                        std::chrono::milliseconds(500)));

    ASSERT_EQ(client.ScanFile("/data/subscription_recovered.bin", &reply), MemRpc::StatusCode::Ok);

    client.Shutdown();
    server.Stop();
    service->OnStop();
}

TEST(VesHealthSubscriptionTest, RecoveryStillWorksWithoutSubscriber) {
    const std::string socketPath =
        "/tmp/ves_health_subscription_none_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<FakeSubscriptionControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest&) {
            ScanFileReply reply;
            reply.code = 0;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClient client(remote);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile("/data/subscription_none.bin", &reply), MemRpc::StatusCode::Ok);

    const int initialOpenCount = service->openCount();
    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; },
                        std::chrono::milliseconds(250)));
    service->SetHealthy(true);
    ASSERT_TRUE(WaitFor([&]() { return service->openCount() > initialOpenCount; },
                        std::chrono::milliseconds(500)));
    ASSERT_EQ(client.ScanFile("/data/subscription_none_recovered.bin", &reply),
              MemRpc::StatusCode::Ok);

    client.Shutdown();
    server.Stop();
    service->OnStop();
}

}  // namespace VirusExecutorService
