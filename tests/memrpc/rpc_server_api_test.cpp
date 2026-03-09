#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/server/handler.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

TEST(RpcServerApiTest, PublicHeaderComposes) {
  MemRpc::RpcServer server;
  MemRpc::ServerOptions options;
  options.high_worker_threads = 2;
  options.normal_worker_threads = 3;
  server.SetOptions(options);
  server.RegisterHandler(MemRpc::Opcode::ScanFile,
                         [](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  MemRpc::RpcEvent event;
  event.event_domain = 1;
  event.event_type = 2;
  event.payload = {1, 2, 3};

  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
  EXPECT_EQ(server.PublishEvent(event), MemRpc::StatusCode::EngineInternalError);
}
