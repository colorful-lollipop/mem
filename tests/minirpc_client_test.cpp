#include <gtest/gtest.h>

#include <memory>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_async_client.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/demo_bootstrap.h"
#include "memrpc/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

TEST(MiniRpcClientTest, SyncAndAsyncCallsRoundTrip) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcAsyncClient async_client(bootstrap);
  ASSERT_EQ(async_client.Init(), MemRpc::StatusCode::Ok);

  EchoRequest echo_request;
  echo_request.text = "ping";
  auto future = async_client.EchoAsync(echo_request);
  MemRpc::RpcReply echo_rpc_reply;
  ASSERT_EQ(future.Wait(&echo_rpc_reply), MemRpc::StatusCode::Ok);
  EchoReply echo_reply;
  ASSERT_TRUE(DecodeEchoReply(echo_rpc_reply.payload, &echo_reply));
  EXPECT_EQ(echo_reply.text, "ping");

  async_client.Shutdown();

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  AddReply add_reply;
  EXPECT_EQ(client.Add(4, 5, &add_reply), MemRpc::StatusCode::Ok);
  EXPECT_EQ(add_reply.sum, 9);

  client.Shutdown();
  server.Stop();
}

TEST(MiniRpcClientTest, HighPrioritySleepCompletesBeforeNormalBacklog) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1});
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcAsyncClient async_client(bootstrap);
  ASSERT_EQ(async_client.Init(), MemRpc::StatusCode::Ok);

  SleepRequest slow_request;
  slow_request.delay_ms = 200;
  auto slow_future = async_client.SleepAsync(slow_request, MemRpc::Priority::Normal, 1000);

  SleepRequest fast_request;
  fast_request.delay_ms = 5;
  MemRpc::RpcReply fast_reply;
  EXPECT_EQ(async_client.SleepAsync(fast_request, MemRpc::Priority::High, 1000).Wait(&fast_reply),
            MemRpc::StatusCode::Ok);

  MemRpc::RpcReply slow_reply;
  EXPECT_EQ(slow_future.Wait(&slow_reply), MemRpc::StatusCode::Ok);

  async_client.Shutdown();
  server.Stop();
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
