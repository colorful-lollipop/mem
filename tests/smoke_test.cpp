#include <gtest/gtest.h>

#include "memrpc/client.h"
#include "memrpc/server.h"

TEST(SmokeTest, TypesConstruct) {
  memrpc::EngineClient client;
  memrpc::EngineServer server;
  (void)client;
  (void)server;
}
