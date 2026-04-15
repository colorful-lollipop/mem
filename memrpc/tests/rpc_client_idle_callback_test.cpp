#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kTestEchoOpcode = static_cast<MemRpc::Opcode>(200);

class CountingBootstrapChannel : public MemRpc::IBootstrapChannel {
public:
    explicit CountingBootstrapChannel(std::shared_ptr<MemRpc::DevBootstrapChannel> delegate)
        : delegate_(std::move(delegate))
    {
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        ++openCount_;
        return delegate_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        ++closeCount_;
        return delegate_->CloseSession();
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        delegate_->SetEngineDeathCallback(std::move(callback));
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
    std::shared_ptr<MemRpc::DevBootstrapChannel> delegate_;
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
};

void CloseHandles(MemRpc::BootstrapHandles& h)
{
    if (h.shmFd >= 0) {
        close(h.shmFd);
    }
    if (h.highReqEventFd >= 0) {
        close(h.highReqEventFd);
    }
    if (h.normalReqEventFd >= 0) {
        close(h.normalReqEventFd);
    }
    if (h.respEventFd >= 0) {
        close(h.respEventFd);
    }
    if (h.reqCreditEventFd >= 0) {
        close(h.reqCreditEventFd);
    }
    if (h.respCreditEventFd >= 0) {
        close(h.respCreditEventFd);
    }
}

void StartEchoServer(const std::shared_ptr<MemRpc::DevBootstrapChannel>& bootstrap, MemRpc::RpcServer* server)
{
    server->SetBootstrapHandles(bootstrap->serverHandles());
    server->RegisterHandler(kTestEchoOpcode,
                            [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
                                reply->status = MemRpc::StatusCode::Ok;
                            });
    ASSERT_EQ(server->Start(), MemRpc::StatusCode::Ok);
}

}  // namespace

namespace MemRpc {

TEST(RpcClientIdleCallbackTest, FiresWhileIdle)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    StartEchoServer(bootstrap, &server);

    RpcClient client(bootstrap);
    std::atomic<int> idle_hits{0};
    RecoveryPolicy policy;
    policy.onIdle = [&](uint64_t idle_ms) {
        if (idle_ms > 0) {
            idle_hits.fetch_add(1);
        }
        return RecoveryDecision{RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), StatusCode::Ok);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && idle_hits.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(idle_hits.load(), 2);
    client.Shutdown();
    server.Stop();
}

TEST(RpcClientIdleCallbackTest, ActivityResetsIdleTimer)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall& call, RpcServerReply* reply) {
        reply->status = StatusCode::Ok;
        reply->payload = call.payload;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    std::atomic<int> long_idle_hits{0};
    RecoveryPolicy policy;
    policy.onIdle = [&](uint64_t idle_ms) {
        if (idle_ms >= 120) {
            long_idle_hits.fetch_add(1);
        }
        return RecoveryDecision{RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    // Keep resetting idle time before it can grow into a longer idle window.
    for (int i = 0; i < 5; ++i) {
        RpcCall call;
        call.opcode = kTestEchoOpcode;
        auto future = client.InvokeAsync(call);
        RpcReply reply;
        std::move(future).Wait(&reply);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_EQ(long_idle_hits.load(), 0);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientIdleCallbackTest, CloseSessionPolicyReopensOnDemand)
{
    auto raw_bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(raw_bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    StartEchoServer(raw_bootstrap, &server);

    auto bootstrap = std::make_shared<CountingBootstrapChannel>(raw_bootstrap);
    RpcClient client(bootstrap);
    std::mutex recoveryEventMutex;
    std::vector<RecoveryEventReport> recoveryEvents;
    client.SetRecoveryEventCallback([&](const RecoveryEventReport& report) {
        std::lock_guard<std::mutex> lock(recoveryEventMutex);
        recoveryEvents.push_back(report);
    });
    RecoveryPolicy policy;
    policy.onIdle = [](uint64_t) { return RecoveryDecision{RecoveryAction::IdleClose, 0}; };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < close_deadline && bootstrap->closeCount() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(bootstrap->closeCount(), 1);
    const auto idleClosedSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(idleClosedSnapshot.lifecycleState, ClientLifecycleState::IdleClosed);
    server.Stop();

    BootstrapHandles prewarmHandles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(raw_bootstrap->OpenSession(prewarmHandles), StatusCode::Ok);
    CloseHandles(prewarmHandles);
    RpcServer restartedServer;
    StartEchoServer(raw_bootstrap, &restartedServer);

    RpcCall call;
    call.opcode = kTestEchoOpcode;
    auto future = client.InvokeAsync(call);
    RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::Ok);
    EXPECT_GE(bootstrap->openCount(), 2);
    const auto reopenedSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(reopenedSnapshot.lifecycleState, ClientLifecycleState::Active);

    const auto reopen_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < reopen_deadline) {
        bool observed = false;
        {
            std::lock_guard<std::mutex> lock(recoveryEventMutex);
            observed = recoveryEvents.size() >= 4;
        }
        if (observed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    RecoveryEventReport initialActiveEvent;
    RecoveryEventReport idleClosedEvent;
    RecoveryEventReport recoveringEvent;
    RecoveryEventReport reopenedEvent;
    {
        std::lock_guard<std::mutex> lock(recoveryEventMutex);
        ASSERT_GE(recoveryEvents.size(), 4u);
        initialActiveEvent = recoveryEvents[0];
        idleClosedEvent = recoveryEvents[1];
        recoveringEvent = recoveryEvents[2];
        reopenedEvent = recoveryEvents.back();
    }
    EXPECT_EQ(initialActiveEvent.state, ClientLifecycleState::Active);
    EXPECT_EQ(idleClosedEvent.state, ClientLifecycleState::IdleClosed);
    EXPECT_EQ(recoveringEvent.state, ClientLifecycleState::Recovering);
    EXPECT_EQ(reopenedEvent.state, ClientLifecycleState::Active);

    client.Shutdown();
    restartedServer.Stop();
}

TEST(RpcClientIdleCallbackTest, LazyInitStartsIdleClosedAndOpensOnDemand)
{
    auto raw_bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(raw_bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    StartEchoServer(raw_bootstrap, &server);

    auto bootstrap = std::make_shared<CountingBootstrapChannel>(raw_bootstrap);
    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(ClientInitMode::LazySession), StatusCode::Ok);
    EXPECT_EQ(bootstrap->openCount(), 0);

    const auto initialSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(initialSnapshot.lifecycleState, ClientLifecycleState::IdleClosed);
    EXPECT_EQ(initialSnapshot.currentSessionId, 0u);

    RpcCall call;
    call.opcode = kTestEchoOpcode;
    RpcReply reply;
    EXPECT_EQ(std::move(client.InvokeAsync(call)).Wait(&reply), StatusCode::Ok);
    EXPECT_EQ(bootstrap->openCount(), 1);

    const auto activeSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(activeSnapshot.lifecycleState, ClientLifecycleState::Active);
    EXPECT_NE(activeSnapshot.currentSessionId, 0u);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientIdleCallbackTest, IdleCloseDoesNotSpamZeroSessionStaleWaitLog)
{
    auto raw_bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(raw_bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    StartEchoServer(raw_bootstrap, &server);

    auto bootstrap = std::make_shared<CountingBootstrapChannel>(raw_bootstrap);
    RpcClient client(bootstrap);
    RecoveryPolicy policy;
    policy.onIdle = [](uint64_t) { return RecoveryDecision{RecoveryAction::IdleClose, 0}; };
    client.SetRecoveryPolicy(std::move(policy));

    testing::internal::CaptureStderr();
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    ASSERT_TRUE([&]() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = client.GetRecoveryRuntimeSnapshot();
            if (bootstrap->closeCount() >= 1 && snapshot.lifecycleState == ClientLifecycleState::IdleClosed &&
                snapshot.currentSessionId == 0) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    client.Shutdown();
    server.Stop();

    const std::string logs = testing::internal::GetCapturedStderr();
    EXPECT_EQ(logs.find("RpcClient::DuplicateResponseWaitHandle clearing stale wait fd: active_session=0 "
                        "current_session=0"),
              std::string::npos)
        << logs;
}

}  // namespace MemRpc
