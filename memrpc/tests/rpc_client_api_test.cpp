#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/server/rpc_server.h"

constexpr MemRpc::Opcode kTestOpcode = 1u;

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
public:
    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        handles = MemRpc::MakeDefaultBootstrapHandles();
        handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
        handles.sessionId = 1;
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

private:
    MemRpc::EngineDeathCallback callback_;
};

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

}  // namespace

// GTest matcher macros inflate cognitive-complexity counts in this API composition check.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(RpcClientApiTest, PublicHeaderComposes)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);
    client.SetEventCallback([](const MemRpc::RpcEvent& event) {
        EXPECT_GE(event.eventDomain, 0u);
        EXPECT_GE(event.eventType, 0u);
    });

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;
    call.payload = std::vector<uint8_t>{1, 2, 3};

    MemRpc::RpcReply reply;
    EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    EXPECT_EQ(call.priority, MemRpc::Priority::Normal);
    EXPECT_EQ(call.execTimeoutMs, 30000u);
    EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);

    auto future = client.InvokeAsync(call);
    EXPECT_TRUE(future.IsReady());
    EXPECT_NE(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);
}
// NOLINTEND(readability-function-cognitive-complexity)

TEST(RpcClientApiTest, PublicHeaderSupportsMoveAwareCallAndReplyApis)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;
    call.payload = std::vector<uint8_t>{4, 5, 6};

    MemRpc::RpcReply reply;
    auto future = client.InvokeAsync(std::move(call));
    EXPECT_TRUE(future.IsReady());
    EXPECT_NE(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);
}

TEST(RpcClientApiTest, InvokeAsyncBeforeInitReturnsClientClosed)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RpcReply reply;
    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    EXPECT_TRUE(future.IsReady());
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::ClientClosed);
    EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);
}

TEST(RpcClientApiTest, ExecTimeoutCanBeConfigured)
{
    MemRpc::RpcCall call;
    call.execTimeoutMs = 500;

    EXPECT_EQ(call.execTimeoutMs, 500u);
}

TEST(RpcClientApiTest, BootstrapHandlesExposeCreditEventFds)
{
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

    EXPECT_EQ(handles.reqCreditEventFd, -1);
    EXPECT_EQ(handles.respCreditEventFd, -1);
}

TEST(RpcClientApiTest, PreInitInvokeDoesNotTriggerRecoveryPolicy)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    std::mutex mutex;
    MemRpc::RpcFailure captured{};
    int calls = 0;
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [&](const MemRpc::RpcFailure& failure) {
        std::lock_guard<std::mutex> lock(mutex);
        captured = failure;
        ++calls;
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;
    call.priority = MemRpc::Priority::Normal;
    call.execTimeoutMs = 1000;

    auto future = client.InvokeAsync(call);
    MemRpc::RpcReply reply;
    const MemRpc::StatusCode status = std::move(future).Wait(&reply);

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(status, MemRpc::StatusCode::ClientClosed);
    EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);
    EXPECT_EQ(calls, 0);
}

TEST(RpcClientApiTest, ShutdownMakesClientPermanentlyTerminal)
{
    auto bootstrap = std::make_shared<FakeBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    client.Shutdown();

    EXPECT_EQ(client.Init(), MemRpc::StatusCode::ClientClosed);

    MemRpc::RpcReply reply;
    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    EXPECT_TRUE(future.IsReady());
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::ClientClosed);
    EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);

    const MemRpc::RecoveryRuntimeSnapshot snapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Closed);
}

TEST(RpcClientApiTest, ShutdownAfterInitStillRejectsInvokeAsyncImmediately)
{
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
    CloseHandles(unusedHandles);

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.RegisterHandler(kTestOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
        reply->status = MemRpc::StatusCode::Ok;
    });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    MemRpc::RpcClient client(bootstrap);

    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);
    client.Shutdown();

    MemRpc::RpcReply reply;
    auto future = client.InvokeAsync(MemRpc::RpcCall{});
    EXPECT_TRUE(future.IsReady());
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::ClientClosed);
    EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);

    server.Stop();
}
