#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

#include "core/session.h"
#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kTestEchoOpcode = static_cast<MemRpc::Opcode>(200);

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

class HealthAwareBootstrapChannel final : public MemRpc::IBootstrapChannel {
public:
    explicit HealthAwareBootstrapChannel(std::shared_ptr<MemRpc::DevBootstrapChannel> delegate)
        : delegate_(std::move(delegate))
    {
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        openCount_.fetch_add(1);
        return delegate_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        closeCount_.fetch_add(1);
        return delegate_->CloseSession();
    }

    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override
    {
        checkCount_.fetch_add(1);
        const auto status = healthStatus_.load(std::memory_order_relaxed);
        if (status == MemRpc::ChannelHealthStatus::SessionMismatch) {
            return {status, expectedSessionId + 1};
        }
        const uint64_t sessionId = status == MemRpc::ChannelHealthStatus::Healthy ? expectedSessionId : 0;
        return {status, sessionId};
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        delegate_->SetEngineDeathCallback(std::move(callback));
    }

    void SetHealthStatus(MemRpc::ChannelHealthStatus status)
    {
        healthStatus_.store(status, std::memory_order_relaxed);
    }

    [[nodiscard]] int openCount() const
    {
        return openCount_.load();
    }
    [[nodiscard]] int closeCount() const
    {
        return closeCount_.load();
    }
    [[nodiscard]] int checkCount() const
    {
        return checkCount_.load();
    }

private:
    std::shared_ptr<MemRpc::DevBootstrapChannel> delegate_;
    std::atomic<MemRpc::ChannelHealthStatus> healthStatus_{MemRpc::ChannelHealthStatus::Unsupported};
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
    std::atomic<int> checkCount_{0};
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

}  // namespace

namespace MemRpc {

TEST(RpcClientTimeoutWatchdogTest, TriggersExecTimeoutForSlowHandler)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
        // Sleep long enough that exec timeout fires.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        reply->status = StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    std::atomic<bool> got_failure{false};
    StatusCode captured_status = StatusCode::Ok;
    RecoveryPolicy policy;
    policy.onFailure = [&](const RpcFailure& failure) {
        got_failure.store(true);
        captured_status = failure.status;
        return RecoveryDecision{RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    RpcCall call;
    call.opcode = kTestEchoOpcode;
    call.execTimeoutMs = 100;  // Short exec timeout.
    auto future = client.InvokeAsync(call);

    RpcReply reply;
    const StatusCode wait_status = std::move(future).Wait(&reply);
    EXPECT_EQ(wait_status, StatusCode::ExecTimeout);
    EXPECT_TRUE(got_failure.load());
    EXPECT_EQ(captured_status, StatusCode::ExecTimeout);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, ClientWaitTimeoutUnblocksWaiterBeforeSlowReplyArrives)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unusedHandles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), StatusCode::Ok);
    CloseHandles(unusedHandles);

    std::atomic<bool> handlerEntered{false};
    std::atomic<bool> handlerExited{false};

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [&](const RpcServerCall&, RpcServerReply* reply) {
        handlerEntered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        handlerExited.store(true);
        reply->status = StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kTestEchoOpcode;
    call.execTimeoutMs = 50;

    const auto start = std::chrono::steady_clock::now();
    auto future = client.InvokeAsync(call);

    RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::ExecTimeout);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(handlerEntered.load());
    EXPECT_LT(elapsedMs, 200);
    EXPECT_FALSE(handlerExited.load());

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, LateReplyAfterClientWaitTimeoutIsDiscarded)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unusedHandles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), StatusCode::Ok);
    CloseHandles(unusedHandles);

    std::atomic<int> callCount{0};

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [&](const RpcServerCall&, RpcServerReply* reply) {
        const int current = ++callCount;
        if (current == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        reply->status = StatusCode::Ok;
        reply->errorCode = current;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall first;
    first.opcode = kTestEchoOpcode;
    first.execTimeoutMs = 50;
    auto firstFuture = client.InvokeAsync(first);

    RpcReply firstReply;
    EXPECT_EQ(std::move(firstFuture).Wait(&firstReply), StatusCode::ExecTimeout);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    RpcCall second;
    second.opcode = kTestEchoOpcode;
    second.execTimeoutMs = 500;
    auto secondFuture = client.InvokeAsync(second);

    RpcReply secondReply;
    EXPECT_EQ(std::move(secondFuture).Wait(&secondReply), StatusCode::Ok);
    EXPECT_EQ(secondReply.errorCode, 2);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, TriggersExecTimeoutWhenStuckInQueue)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(unused_handles);

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    std::atomic<bool> server_started{false};
    server.RegisterHandler(kTestEchoOpcode, [&](const RpcServerCall&, RpcServerReply* reply) {
        server_started.store(true);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        reply->status = StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    // First request: blocks the server worker.
    RpcCall blocker;
    blocker.opcode = kTestEchoOpcode;
    blocker.execTimeoutMs = 10000;
    auto blocker_future = client.InvokeAsync(blocker);

    // Wait until server picks up the blocker.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!server_started.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(server_started.load());

    // Second request: will sit in queue since the worker is busy.
    std::atomic<bool> got_failure{false};
    StatusCode captured_status = StatusCode::Ok;
    RecoveryPolicy policy2;
    policy2.onFailure = [&](const RpcFailure& failure) {
        if (failure.status == StatusCode::ExecTimeout) {
            got_failure.store(true);
            captured_status = failure.status;
        }
        return RecoveryDecision{RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy2));

    RpcCall queued_call;
    queued_call.opcode = kTestEchoOpcode;
    queued_call.execTimeoutMs = 100;
    auto queued_future = client.InvokeAsync(queued_call);

    RpcReply reply;
    const StatusCode wait_status = std::move(queued_future).Wait(&reply);
    EXPECT_EQ(wait_status, StatusCode::ExecTimeout);
    EXPECT_TRUE(got_failure.load());
    EXPECT_EQ(captured_status, StatusCode::ExecTimeout);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, UnsupportedHealthCheckDoesNotRestartSession)
{
    auto rawBootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
    CloseHandles(unusedHandles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(rawBootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
        reply->status = MemRpc::StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    auto bootstrap = std::make_shared<HealthAwareBootstrapChannel>(rawBootstrap);
    bootstrap->SetHealthStatus(MemRpc::ChannelHealthStatus::Unsupported);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ASSERT_TRUE(WaitFor([&]() { return bootstrap->checkCount() >= 2; }, std::chrono::milliseconds(500)));
    EXPECT_EQ(bootstrap->closeCount(), 0);
    EXPECT_EQ(bootstrap->openCount(), 1);

    MemRpc::RpcCall call;
    call.opcode = kTestEchoOpcode;
    auto future = client.InvokeAsync(call);
    MemRpc::RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);

    client.Shutdown();
    server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, HealthFailuresTriggerWatchdogRestart)
{
    constexpr MemRpc::ChannelHealthStatus kSignals[] = {
        MemRpc::ChannelHealthStatus::Timeout,
        MemRpc::ChannelHealthStatus::Malformed,
        MemRpc::ChannelHealthStatus::Unhealthy,
        MemRpc::ChannelHealthStatus::SessionMismatch,
    };

    for (MemRpc::ChannelHealthStatus signal : kSignals) {
        auto rawBootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
        MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
        CloseHandles(unusedHandles);

        MemRpc::RpcServer server;
        server.SetBootstrapHandles(rawBootstrap->serverHandles());
        server.RegisterHandler(kTestEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
            reply->status = MemRpc::StatusCode::Ok;
        });
        ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

        auto bootstrap = std::make_shared<HealthAwareBootstrapChannel>(rawBootstrap);
        bootstrap->SetHealthStatus(MemRpc::ChannelHealthStatus::Healthy);

        MemRpc::RpcClient client(bootstrap);
        ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

        bootstrap->SetHealthStatus(signal);
        ASSERT_TRUE(WaitFor([&]() { return bootstrap->closeCount() >= 1; }, std::chrono::milliseconds(500)));
        bootstrap->SetHealthStatus(MemRpc::ChannelHealthStatus::Healthy);
        ASSERT_TRUE(WaitFor([&]() { return bootstrap->openCount() >= 2; }, std::chrono::milliseconds(500)));

        MemRpc::RpcCall call;
        call.opcode = kTestEchoOpcode;
        auto future = client.InvokeAsync(call);
        MemRpc::RpcReply reply;
        EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);

        client.Shutdown();
        server.Stop();
    }
}

TEST(RpcClientTimeoutWatchdogTest, LongRunningExecutionStillEndsAsExecTimeout)
{
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
    CloseHandles(unusedHandles);

    MemRpc::ServerOptions options;
    options.executionHeartbeatIntervalMs = 10;
    MemRpc::RpcServer server({}, options);
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestEchoOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        reply->status = MemRpc::StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kTestEchoOpcode;
    call.execTimeoutMs = 120;
    auto future = client.InvokeAsync(call);

    MemRpc::RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::ExecTimeout);

    client.Shutdown();
    server.Stop();
}

}  // namespace MemRpc
