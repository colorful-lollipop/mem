#include <gtest/gtest.h>

#include <type_traits>

#include "apps/minirpc/common/minirpc_types.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;
namespace MiniRpc = OHOS::Security::VirusProtectionService::MiniRpc;

TEST(FrameworkSplitHeadersTest, NewHeaderLayoutComposes) {
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
  EXPECT_TRUE((std::is_default_constructible_v<MiniRpc::EchoRequest>));
  EXPECT_TRUE((std::is_class_v<MiniRpc::MiniRpcClient>));
}
