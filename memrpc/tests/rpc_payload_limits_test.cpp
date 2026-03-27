#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <memory>
#include <vector>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kPayloadLimitOpcode = 301;

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

}  // namespace

namespace MemRpc {

TEST(RpcPayloadLimitsTest, OversizedRequestFailsBeforeSubmission)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), StatusCode::Ok);
    CloseHandles(handles);

    RpcServer server(bootstrap->serverHandles());
    server.RegisterHandler(kPayloadLimitOpcode, [](const RpcServerCall& call, RpcServerReply* reply) {
        reply->status = StatusCode::Ok;
        reply->payload = call.payload;
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kPayloadLimitOpcode;
    call.payload.resize(DEFAULT_MAX_REQUEST_BYTES + 1U, 0x5a);

    RpcReply reply;
    EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), StatusCode::PayloadTooLarge);
    EXPECT_EQ(reply.status, StatusCode::PayloadTooLarge);

    client.Shutdown();
    server.Stop();
}

TEST(RpcPayloadLimitsTest, OversizedResponseReturnsPayloadTooLargeWithoutRetry)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), StatusCode::Ok);
    CloseHandles(handles);

    std::atomic<int> callCount{0};
    RpcServer server(bootstrap->serverHandles());
    server.RegisterHandler(kPayloadLimitOpcode, [&callCount](const RpcServerCall&, RpcServerReply* reply) {
        callCount.fetch_add(1, std::memory_order_relaxed);
        reply->status = StatusCode::Ok;
        reply->payload.resize(DEFAULT_MAX_RESPONSE_BYTES + 1U, 0x7b);
    });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kPayloadLimitOpcode;
    call.payload = {1, 2, 3};

    RpcReply reply;
    EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), StatusCode::PayloadTooLarge);
    EXPECT_EQ(reply.status, StatusCode::PayloadTooLarge);
    EXPECT_TRUE(reply.payload.empty());
    EXPECT_EQ(callCount.load(std::memory_order_relaxed), 1);

    client.Shutdown();
    server.Stop();
}

TEST(RpcPayloadLimitsTest, PublishEventRejectsOversizedPayload)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), StatusCode::Ok);
    CloseHandles(handles);

    RpcServer server(bootstrap->serverHandles());
    server.RegisterHandler(kPayloadLimitOpcode,
                           [](const RpcServerCall&, RpcServerReply* reply) { reply->status = StatusCode::Ok; });
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcEvent event;
    event.eventDomain = 7;
    event.eventType = 11;
    event.payload.resize(DEFAULT_MAX_RESPONSE_BYTES + 1U, 0x33);

    EXPECT_EQ(server.PublishEvent(event), StatusCode::PayloadTooLarge);

    server.Stop();
}

}  // namespace MemRpc
