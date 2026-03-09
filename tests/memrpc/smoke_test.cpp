#include <gtest/gtest.h>

#include "memrpc/rpc_client.h"
#include "memrpc/rpc_server.h"

TEST(SmokeTest, TypesConstruct) {
  memrpc::RpcClient client;
  memrpc::RpcServer server;
  (void)client;
  (void)server;
}
