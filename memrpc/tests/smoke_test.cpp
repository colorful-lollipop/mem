#include <gtest/gtest.h>

#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

TEST(SmokeTest, TypesConstruct)
{
    MemRpc::RpcClient client;
    MemRpc::RpcServer server;
    (void)client;
    (void)server;
}
