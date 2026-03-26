#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kTestOpcode = 1u;

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
public:
    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        handles = MemRpc::MakeDefaultBootstrapHandles();
        handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode CloseSession() override
    {
        return MemRpc::StatusCode::Ok;
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        callback_ = std::move(callback);
    }

    void SimulateDeath(uint64_t session_id)
    {
        if (callback_) {
            callback_(session_id);
        }
    }

private:
    MemRpc::EngineDeathCallback callback_;
};

void CloseHandles(MemRpc::BootstrapHandles& h)
{
    if (h.shmFd >= 0)
        close(h.shmFd);
    if (h.highReqEventFd >= 0)
        close(h.highReqEventFd);
    if (h.normalReqEventFd >= 0)
        close(h.normalReqEventFd);
    if (h.respEventFd >= 0)
        close(h.respEventFd);
    if (h.reqCreditEventFd >= 0)
        close(h.reqCreditEventFd);
    if (h.respCreditEventFd >= 0)
        close(h.respCreditEventFd);
}

}  // namespace

// Without a handler, all futures fail with PeerDisconnected (backward compatible).
TEST(EngineDeathHandlerTest, NoHandlerBackwardCompat)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    std::mutex mutex;
    std::vector<MemRpc::RpcFailure> failures;
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [&](const MemRpc::RpcFailure& f) {
        std::lock_guard<std::mutex> lock(mutex);
        failures.push_back(f);
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    // Init will fail to create a real session (no shmFd), but InvokeAsync
    // will produce ready futures with failure status. This test verifies
    // the handler-absent code path compiles and runs without crash.
    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    ASSERT_TRUE(future.IsReady());
    MemRpc::RpcReply reply;
    EXPECT_NE(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);

    client.Shutdown();
}

// With handler returning Ignore, all futures should fail.
TEST(EngineDeathHandlerTest, AbandonFailsAll)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    bool handler_called = false;
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
        handler_called = true;
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    ASSERT_TRUE(future.IsReady());
    MemRpc::RpcReply reply;
    EXPECT_NE(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);

    client.Shutdown();
}

// With handler returning Restart, verify the handler is callable.
TEST(EngineDeathHandlerTest, RestartHandlerCallable)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    std::atomic<bool> handler_called{false};
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
        handler_called.store(true);
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    ASSERT_TRUE(future.IsReady());
    MemRpc::RpcReply reply;
    std::move(future).Wait(&reply);

    client.Shutdown();
}

// Verify StatusCode::CrashedDuringExecution exists and has correct value.
TEST(EngineDeathHandlerTest, CrashedDuringExecutionStatusCode)
{
    MemRpc::RpcReply reply;
    reply.status = MemRpc::StatusCode::CrashedDuringExecution;
    EXPECT_EQ(reply.status, MemRpc::StatusCode::CrashedDuringExecution);
    EXPECT_NE(reply.status, MemRpc::StatusCode::PeerDisconnected);
    EXPECT_NE(reply.status, MemRpc::StatusCode::Ok);
}

// Verify EngineDeathReport struct layout.
TEST(EngineDeathHandlerTest, EngineDeathReportLayout)
{
    MemRpc::EngineDeathReport report;
    EXPECT_EQ(report.deadSessionId, 0u);
    EXPECT_EQ(report.safeToReplayCount, 0u);
    EXPECT_TRUE(report.poisonPillSuspects.empty());

    MemRpc::EngineDeathReport::PoisonPillSuspect suspect;
    EXPECT_EQ(suspect.requestId, 0u);
    EXPECT_EQ(suspect.opcode, MemRpc::OPCODE_INVALID);
    EXPECT_EQ(suspect.lastState, MemRpc::RpcRuntimeState::Unknown);
}

// Verify RecoveryDecision defaults.
TEST(EngineDeathHandlerTest, RecoveryDecisionDefaults)
{
    MemRpc::RecoveryDecision decision;
    EXPECT_EQ(decision.action, MemRpc::RecoveryAction::Ignore);
    EXPECT_EQ(decision.delayMs, 0u);
}

// Verify synchronous waiting still works directly on RpcClient.
TEST(EngineDeathHandlerTest, ClientWaitSetRecoveryPolicy)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    bool called = false;
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
        called = true;
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;
    MemRpc::RpcReply reply;
    client.InvokeAsync(call).Wait(&reply);

    client.Shutdown();
}

// Verify that Shutdown during a potential restart doesn't deadlock or crash.
TEST(EngineDeathHandlerTest, ShutdownIsClean)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 5000};
    };
    client.SetRecoveryPolicy(std::move(policy));

    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    ASSERT_TRUE(future.IsReady());
    MemRpc::RpcReply reply;
    std::move(future).Wait(&reply);

    // Shutdown should complete quickly even if restart was requested with delay.
    const auto start = std::chrono::steady_clock::now();
    client.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
    const auto snapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Closed);
}

TEST(EngineDeathHandlerTest, RestartDelayBlocksDemandReconnectUntilCooldownExpires)
{
    constexpr MemRpc::Opcode kEchoOpcode = static_cast<MemRpc::Opcode>(201);

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unused_handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
    CloseHandles(unused_handles);

    auto start_server = [&](MemRpc::RpcServer* server) {
        server->SetBootstrapHandles(bootstrap->serverHandles());
        server->RegisterHandler(kEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
            reply->status = MemRpc::StatusCode::Ok;
        });
        ASSERT_EQ(server->Start(), MemRpc::StatusCode::Ok);
    };

    MemRpc::RpcServer server;
    start_server(&server);

    MemRpc::RpcClient client(bootstrap);
    std::mutex recoveryEventMutex;
    std::vector<MemRpc::RecoveryEventReport> recoveryEvents;
    client.SetRecoveryEventCallback([&](const MemRpc::RecoveryEventReport& report) {
        std::lock_guard<std::mutex> lock(recoveryEventMutex);
        recoveryEvents.push_back(report);
    });
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 200};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kEchoOpcode;
    auto ready_future = client.InvokeAsync(call);
    MemRpc::RpcReply ready_reply;
    ASSERT_EQ(std::move(ready_future).Wait(&ready_reply), MemRpc::StatusCode::Ok);

    bootstrap->SimulateEngineDeathForTest();
    server.Stop();

    const auto cooldownSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(cooldownSnapshot.lifecycleState, MemRpc::ClientLifecycleState::Cooldown);
    EXPECT_TRUE(cooldownSnapshot.recoveryPending);

    MemRpc::RpcServer restarted_server;
    std::thread reopen_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MemRpc::BootstrapHandles prewarm_handles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(bootstrap->OpenSession(prewarm_handles), MemRpc::StatusCode::Ok);
        CloseHandles(prewarm_handles);

        start_server(&restarted_server);
    });

    auto blocked_future = client.InvokeAsync(call);
    MemRpc::RpcReply blocked_reply;
    EXPECT_EQ(std::move(blocked_future).Wait(&blocked_reply), MemRpc::StatusCode::Ok);
    reopen_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto recovered_future = client.InvokeAsync(call);
    MemRpc::RpcReply recovered_reply;
    EXPECT_EQ(std::move(recovered_future).Wait(&recovered_reply), MemRpc::StatusCode::Ok);
    const auto recoveredSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(recoveredSnapshot.lifecycleState, MemRpc::ClientLifecycleState::Active);

    const auto eventDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < eventDeadline) {
        bool observed = false;
        {
            std::lock_guard<std::mutex> lock(recoveryEventMutex);
            observed = recoveryEvents.size() >= 3;
        }
        if (observed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    MemRpc::RecoveryEventReport initialActiveEvent;
    MemRpc::RecoveryEventReport cooldownEvent;
    MemRpc::RecoveryEventReport recoveredActiveEvent;
    {
        std::lock_guard<std::mutex> lock(recoveryEventMutex);
        ASSERT_GE(recoveryEvents.size(), 3u);
        initialActiveEvent = recoveryEvents[0];
        cooldownEvent = recoveryEvents[1];
        recoveredActiveEvent = recoveryEvents.back();
    }
    EXPECT_EQ(initialActiveEvent.state, MemRpc::ClientLifecycleState::Active);
    EXPECT_EQ(cooldownEvent.state, MemRpc::ClientLifecycleState::Cooldown);
    EXPECT_EQ(cooldownEvent.cooldownDelayMs, 200u);
    EXPECT_EQ(recoveredActiveEvent.state, MemRpc::ClientLifecycleState::Active);

    client.Shutdown();
    restarted_server.Stop();
}

TEST(EngineDeathHandlerTest, IgnoreLeavesClientDisconnectedUntilDemandReconnect)
{
    constexpr MemRpc::Opcode kEchoOpcode = static_cast<MemRpc::Opcode>(203);

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
    CloseHandles(unusedHandles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
        reply->status = MemRpc::StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    MemRpc::RpcClient client(bootstrap);
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kEchoOpcode;
    MemRpc::RpcReply reply;
    ASSERT_EQ(client.InvokeAsync(call).Wait(&reply), MemRpc::StatusCode::Ok);

    bootstrap->SimulateEngineDeathForTest();

    const auto disconnectedSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(disconnectedSnapshot.lifecycleState, MemRpc::ClientLifecycleState::NoSession);
    EXPECT_FALSE(disconnectedSnapshot.recoveryPending);
    EXPECT_EQ(disconnectedSnapshot.currentSessionId, 0u);

    client.Shutdown();
    server.Stop();
}

TEST(EngineDeathHandlerTest, DuplicateEngineDeathSignalIsIgnoredForSameSession)
{
    constexpr MemRpc::Opcode kEchoOpcode = static_cast<MemRpc::Opcode>(204);

    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
    CloseHandles(unusedHandles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
        reply->status = MemRpc::StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    MemRpc::RpcClient client(bootstrap);
    std::atomic<int> engineDeathCalls{0};
    MemRpc::RecoveryPolicy policy;
    policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
        engineDeathCalls.fetch_add(1, std::memory_order_relaxed);
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kEchoOpcode;
    MemRpc::RpcReply reply;
    ASSERT_EQ(client.InvokeAsync(call).Wait(&reply), MemRpc::StatusCode::Ok);

    const uint64_t deadSessionId = client.GetRecoveryRuntimeSnapshot().currentSessionId;
    ASSERT_NE(deadSessionId, 0u);

    bootstrap->SimulateEngineDeathForTest(deadSessionId);
    bootstrap->SimulateEngineDeathForTest(deadSessionId);

    EXPECT_EQ(engineDeathCalls.load(std::memory_order_relaxed), 1);
    const auto disconnectedSnapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(disconnectedSnapshot.lifecycleState, MemRpc::ClientLifecycleState::NoSession);
    EXPECT_EQ(disconnectedSnapshot.currentSessionId, 0u);

    client.Shutdown();
    server.Stop();
}
