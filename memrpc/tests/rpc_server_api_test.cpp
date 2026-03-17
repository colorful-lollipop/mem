#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/server/handler.h"
#include "memrpc/server/rpc_server.h"

constexpr MemRpc::Opcode kTestOpcode = 1u;

TEST(RpcServerApiTest, PublicHeaderComposes) {
  MemRpc::RpcServer server;
  MemRpc::ServerOptions options;
  options.executionHeartbeatIntervalMs = 9;
  options.highWorkerThreads = 2;
  options.normalWorkerThreads = 3;
  options.completionQueueCapacity = 4;
  MemRpc::RpcServerRuntimeStats stats;
  server.SetOptions(options);
  server.RegisterHandler(kTestOpcode,
                         [](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  MemRpc::RpcEvent event;
  event.eventDomain = 1;
  event.eventType = 2;
  event.payload = {1, 2, 3};

  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
  EXPECT_EQ(stats.completionBacklog, 0u);
  EXPECT_EQ(stats.activeRequestExecutions, 0u);
  EXPECT_EQ(stats.oldestExecutionAgeMs, 0u);
  EXPECT_EQ(server.PublishEvent(event), MemRpc::StatusCode::EngineInternalError);
}
