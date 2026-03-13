#include <gtest/gtest.h>

#include <fstream>
#include <type_traits>

#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

#define MEMRPC_SOURCE_PATH(rel) MEMRPC_SOURCE_DIR rel
#define MEMRPC_REPO_PATH(rel) MEMRPC_REPO_ROOT rel

TEST(FrameworkSplitHeadersTest, NewHeaderLayoutComposes) {
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
}

TEST(FrameworkSplitHeadersTest, MainlineHeadersDoNotDependOnLegacyCompatPaths) {
  std::ifstream clientHeader(MEMRPC_SOURCE_PATH("/include/memrpc/client/rpc_client.h"));
  ASSERT_TRUE(clientHeader.is_open());
  std::string clientContent((std::istreambuf_iterator<char>(clientHeader)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(clientContent.find("compat/"), std::string::npos);
  EXPECT_EQ(clientContent.find("vps_demo"), std::string::npos);
  EXPECT_EQ(clientContent.find("minirpc"), std::string::npos);

  std::ifstream testkitHeader(MEMRPC_REPO_PATH("/include/vpsdemo/testkit/testkit_client.h"));
  ASSERT_TRUE(testkitHeader.is_open());
  std::string testkitContent((std::istreambuf_iterator<char>(testkitHeader)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(testkitContent.find("vps_demo"), std::string::npos);
}
