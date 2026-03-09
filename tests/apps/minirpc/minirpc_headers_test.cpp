#include <gtest/gtest.h>

#include <type_traits>

#include "apps/minirpc/common/minirpc_types.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/rpc_client.h"

TEST(MiniRpcHeadersTest, NewHeaderLayoutComposes) {
  EXPECT_TRUE((std::is_default_constructible_v<
               OHOS::Security::VirusProtectionService::MiniRpc::EchoRequest>));
  EXPECT_TRUE((std::is_class_v<
               OHOS::Security::VirusProtectionService::MiniRpc::MiniRpcClient>));
  EXPECT_FALSE((std::is_copy_constructible_v<
                OHOS::Security::VirusProtectionService::MemRpc::RpcClient>));
}
