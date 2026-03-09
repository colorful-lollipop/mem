#include <gtest/gtest.h>

#include <fstream>
#include <type_traits>

#include "apps/minirpc/common/minirpc_types.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;
namespace MiniRpc = OHOS::Security::VirusProtectionService::MiniRpc;

TEST(FrameworkSplitHeadersTest, NewHeaderLayoutComposes) {
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
  EXPECT_TRUE((std::is_default_constructible_v<MiniRpc::EchoRequest>));
  EXPECT_TRUE((std::is_class_v<MiniRpc::MiniRpcClient>));
}

TEST(FrameworkSplitHeadersTest, MainlineHeadersDoNotDependOnLegacyCompatPaths) {
  std::ifstream clientHeader("/root/code/demo/mem/include/memrpc/client/rpc_client.h");
  ASSERT_TRUE(clientHeader.is_open());
  std::string clientContent((std::istreambuf_iterator<char>(clientHeader)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(clientContent.find("compat/"), std::string::npos);
  EXPECT_EQ(clientContent.find("vps_demo"), std::string::npos);

  std::ifstream minirpcHeader("/root/code/demo/mem/include/apps/minirpc/parent/minirpc_client.h");
  ASSERT_TRUE(minirpcHeader.is_open());
  std::string minirpcContent((std::istreambuf_iterator<char>(minirpcHeader)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(minirpcContent.find("vps_demo"), std::string::npos);
}

TEST(FrameworkSplitHeadersTest, TopLevelClientAndServerHeadersFollowMainlineTypes) {
  EXPECT_TRUE((std::is_same_v<MemRpc::RpcClient, ::memrpc::RpcClient>));
  EXPECT_TRUE((std::is_same_v<MemRpc::RpcServer, ::memrpc::RpcServer>));
}
