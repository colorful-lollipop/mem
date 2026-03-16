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
#include "iservice_registry.h"
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

bool WaitForRecoveredScan(VesClient* client,
                          const std::string& path,
                          ScanFileReply* reply,
                          std::chrono::milliseconds timeout,
                          MemRpc::StatusCode* lastStatus = nullptr)
{
    if (client == nullptr || reply == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    MemRpc::StatusCode status = MemRpc::StatusCode::InvalidArgument;
    ScanTask task{path};
    while (std::chrono::steady_clock::now() < deadline) {
        status = client->ScanFile(task, reply);
        if (status == MemRpc::StatusCode::Ok) {
            if (lastStatus != nullptr) {
                *lastStatus = status;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (lastStatus != nullptr) {
        *lastStatus = status;
    }
    return false;
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

void UnloadControlService()
{
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam != nullptr) {
        (void)sam->UnloadSystemAbility(VES_CONTROL_SA_ID);
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

    MemRpc::StatusCode AnyCall(const VesAnyCallRequest&, VesAnyCallReply&) override
    {
        return MemRpc::StatusCode::InvalidArgument;
    }

    void OnStart() override {}

    void OnStop() override
    {
        sessionOpen_.store(false);
        (void)bootstrap_->CloseSession();
        OHOS::SystemAbility::OnStop();
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
    UnloadControlService();
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
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, DeliversPushedEventsWhenSubscribed) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_pushed_events_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClient client(remote);
    std::atomic<int> eventCount{0};
    std::atomic<uint32_t> latestDomain{0};
    std::atomic<uint32_t> latestType{0};
    std::atomic<size_t> latestPayloadSize{0};
    client.SetEventCallback([&](const MemRpc::RpcEvent& event) {
        latestDomain.store(event.eventDomain);
        latestType.store(event.eventType);
        latestPayloadSize.store(event.payload.size());
        eventCount.fetch_add(1);
    });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ASSERT_TRUE(WaitFor([&]() { return eventCount.load() > 0; },
                        std::chrono::milliseconds(1000)));
    EXPECT_EQ(latestDomain.load(), VES_EVENT_DOMAIN_RUNTIME);
    EXPECT_GE(latestType.load(), static_cast<uint32_t>(VesEventType::RandomScanResult));
    EXPECT_LE(latestType.load(), static_cast<uint32_t>(VesEventType::RandomLifecycle));
    EXPECT_GT(latestPayloadSize.load(), 0u);

    client.Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, AllowsManualBlockingPublishFromServiceThread) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_manual_pushed_events_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClient client(remote);
    constexpr uint32_t kManualEventType = static_cast<uint32_t>(VesEventType::RandomLifecycle);
    const std::string manualPayload = "manual-blocking-event";
    std::atomic<int> matchedEventCount{0};
    client.SetEventCallback([&](const MemRpc::RpcEvent& event) {
        const std::string payload(event.payload.begin(), event.payload.end());
        if (event.eventDomain == VES_EVENT_DOMAIN_RUNTIME &&
            event.eventType == kManualEventType &&
            payload == manualPayload) {
            matchedEventCount.fetch_add(1);
        }
    });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::StatusCode publishStatus = MemRpc::StatusCode::InvalidArgument;
    std::thread publisher([&] {
        publishStatus = service->service().PublishTextEvent(kManualEventType, manualPayload);
    });
    publisher.join();

    EXPECT_EQ(publishStatus, MemRpc::StatusCode::Ok);
    ASSERT_TRUE(WaitFor([&]() { return matchedEventCount.load() > 0; },
                        std::chrono::milliseconds(1000)));

    client.Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, OversizedScanRequestFallsBackToAnyCall) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_health_subscription_anycall_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();
    VesClient client(remote);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    const std::string longPath =
        "/data/" + std::string(MemRpc::DEFAULT_MAX_REQUEST_BYTES + 64, 'a');
    ScanTask task{longPath};
    ScanFileReply reply;
    EXPECT_EQ(client.ScanFile(task, &reply, MemRpc::Priority::High), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.code, 0);

    client.Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, RejectsNullReply) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_health_subscription_null_task_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();
    VesClient client(remote);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanTask task{"/data/clean.apk"};
    EXPECT_EQ(client.ScanFile(task, nullptr), MemRpc::StatusCode::InvalidArgument);

    client.Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, BlockingSubscriberDoesNotBlockRecovery) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_health_subscription_blocking_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<FakeSubscriptionControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanTask&) {
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

    ScanTask subscriptionTask{"/data/subscription.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile(subscriptionTask, &reply), MemRpc::StatusCode::Ok);

    const int initialOpenCount = service->openCount();
    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; },
                        std::chrono::milliseconds(250)));
    service->SetHealthy(true);
    ASSERT_TRUE(WaitFor([&]() { return service->openCount() > initialOpenCount; },
                        std::chrono::milliseconds(500)));
    MemRpc::StatusCode recoveredStatus = MemRpc::StatusCode::InvalidArgument;
    ASSERT_TRUE(WaitForRecoveredScan(&client, "/data/subscription_recovered.bin", &reply,
                                     std::chrono::milliseconds(500), &recoveredStatus))
        << "last status=" << static_cast<int>(recoveredStatus);

    client.Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

TEST(VesHealthSubscriptionTest, RecoveryStillWorksWithoutSubscriber) {
    UnloadControlService();
    const std::string socketPath =
        "/tmp/ves_health_subscription_none_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<FakeSubscriptionControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanTask&) {
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

    ScanTask subscriptionNoneTask{"/data/subscription_none.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile(subscriptionNoneTask, &reply), MemRpc::StatusCode::Ok);

    const int initialOpenCount = service->openCount();
    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; },
                        std::chrono::milliseconds(250)));
    service->SetHealthy(true);
    ASSERT_TRUE(WaitFor([&]() { return service->openCount() > initialOpenCount; },
                        std::chrono::milliseconds(500)));
    MemRpc::StatusCode recoveredStatus = MemRpc::StatusCode::InvalidArgument;
    ASSERT_TRUE(WaitForRecoveredScan(&client, "/data/subscription_none_recovered.bin", &reply,
                                     std::chrono::milliseconds(500), &recoveredStatus))
        << "last status=" << static_cast<int>(recoveredStatus);

    client.Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

}  // namespace VirusExecutorService
