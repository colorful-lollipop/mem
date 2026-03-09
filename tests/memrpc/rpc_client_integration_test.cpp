#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "memrpc/rpc_client.h"
#include "memrpc/rpc_server.h"
#include "memrpc/sa_bootstrap.h"

TEST(RpcClientIntegrationTest, InvokeAsyncAndInvokeSyncRoundTrip) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->engine_code = 7;
                           reply->detail_code = 9;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  client.SetEventCallback([](const memrpc::RpcEvent& event) {
    EXPECT_GE(event.event_domain, 0u);
    EXPECT_GE(event.event_type, 0u);
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{4, 5, 6};

  auto future = client.InvokeAsync(call);
  memrpc::RpcReply async_reply;
  EXPECT_EQ(future.Wait(&async_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(async_reply.payload, call.payload);
  EXPECT_EQ(async_reply.engine_code, 7);
  EXPECT_EQ(async_reply.detail_code, 9);

  memrpc::RpcReply sync_reply;
  EXPECT_EQ(client.InvokeSync(call, &sync_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(sync_reply.payload, call.payload);
  EXPECT_EQ(sync_reply.engine_code, 7);
  EXPECT_EQ(sync_reply.detail_code, 9);

  client.Shutdown();
  server.Stop();
}
