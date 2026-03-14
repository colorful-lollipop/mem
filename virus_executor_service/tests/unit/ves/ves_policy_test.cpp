#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <unistd.h>

#include "client/ves_client.h"
#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "service/virus_executor_service.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"
#include "system_ability.h"
#include "transport/ves_control_stub.h"

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

class FakeHealthControlService final : public OHOS::SystemAbility,
                                       public VesControlStub {
 public:
    explicit FakeHealthControlService(const std::string& socketPath)
        : OHOS::SystemAbility(VES_CONTROL_SA_ID, true),
          bootstrap_(std::make_shared<MemRpc::DevBootstrapChannel>())
    {
        AsObject()->SetServicePath(socketPath);
    }

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
        reply.version = 1;
        reply.sessionId = sessionOpen_.load() ? bootstrap_->serverHandles().sessionId : 0;
        std::snprintf(reply.currentTask, sizeof(reply.currentTask), "%s", "idle");
        reply.status = static_cast<uint32_t>(
            sessionOpen_.load() && healthy_.load() ? VesHeartbeatStatus::Ok
                                                   : VesHeartbeatStatus::Unhealthy);
        return MemRpc::StatusCode::Ok;
    }

    void OnStart() override {}

    void OnStop() override
    {
        sessionOpen_.store(false);
        (void)bootstrap_->CloseSession();
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

TEST(VesPolicyTest, ExecTimeoutTriggersOnFailure) {
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unused{};
    ASSERT_EQ(bootstrap->OpenSession(unused), MemRpc::StatusCode::Ok);

    MemRpc::RpcServer server(bootstrap->serverHandles());
    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest& req) {
            // Force exec timeout by sleeping longer than client timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = 0;
            (void)req;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    std::atomic<bool> failureCalled{false};
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [&](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            failureCalled.store(true);
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanFileRequest req;
    req.filePath = "/data/sleep50.bin";

    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(VesOpcode::ScanFile);
    call.execTimeoutMs = 5;  // short timeout
    MemRpc::CodecTraits<ScanFileRequest>::Encode(req, &call.payload);

    auto future = client.InvokeAsync(call);
    MemRpc::RpcReply reply;
    auto status = future.Wait(&reply);

    EXPECT_EQ(status, MemRpc::StatusCode::ExecTimeout);
    EXPECT_TRUE(failureCalled.load());

    client.Shutdown();
    server.Stop();
}

TEST(VesPolicyTest, IdleShutdownClosesSessionAndReopensOnDemand) {
    const std::string socketPath =
        "/tmp/ves_idle_policy_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto remote = std::make_shared<OHOS::IRemoteObject>();
    remote->SetSaId(VES_CONTROL_SA_ID);
    remote->SetServicePath(socketPath);

    VesClient::RegisterProxyFactory();

    VesClientOptions options;
    options.idleShutdownTimeoutMs = 80;
    VesClient client(remote, options);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile("/data/clean.apk", &reply), MemRpc::StatusCode::Ok);

    VesHeartbeatReply heartbeat{};
    const auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < idleDeadline) {
        ASSERT_EQ(service->Heartbeat(heartbeat), MemRpc::StatusCode::Ok);
        if (heartbeat.sessionId == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(heartbeat.sessionId, 0u);

    ASSERT_EQ(client.ScanFile("/data/reopen.apk", &reply), MemRpc::StatusCode::Ok);

    ASSERT_EQ(service->Heartbeat(heartbeat), MemRpc::StatusCode::Ok);
    EXPECT_NE(heartbeat.sessionId, 0u);

    client.Shutdown();
    service->OnStop();
}

TEST(VesPolicyTest, VesClientRecoversFromHeartbeatFailureWithoutClientLoop) {
    const std::string socketPath =
        "/tmp/ves_watchdog_policy_" + std::to_string(getpid()) + ".sock";

    auto service = std::make_shared<FakeHealthControlService>(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest&) {
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = 0;
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
    ASSERT_EQ(client.ScanFile("/data/healthy.bin", &reply), MemRpc::StatusCode::Ok);

    const int initialOpenCount = service->openCount();
    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; },
                        std::chrono::milliseconds(500)));
    service->SetHealthy(true);
    ASSERT_TRUE(WaitFor([&]() { return service->openCount() > initialOpenCount; },
                        std::chrono::milliseconds(500)));

    ASSERT_EQ(client.ScanFile("/data/recovered.bin", &reply), MemRpc::StatusCode::Ok);
    EXPECT_FALSE(client.EngineDied());

    client.Shutdown();
    server.Stop();
    service->OnStop();
}

}  // namespace VirusExecutorService
