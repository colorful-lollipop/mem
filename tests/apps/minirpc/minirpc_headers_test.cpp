#include <gtest/gtest.h>

#include <type_traits>

#include "apps/minirpc/common/minirpc_types.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "apps/minirpc/protocol.h"
#include "memrpc/client/rpc_client.h"

using OHOS::Security::VirusProtectionService::MiniRpc::MiniRpcOpcode;

TEST(MiniRpcHeadersTest, NewHeaderLayoutComposes) {
  EXPECT_TRUE((std::is_default_constructible_v<
               OHOS::Security::VirusProtectionService::MiniRpc::EchoRequest>));
  EXPECT_TRUE((std::is_class_v<
               OHOS::Security::VirusProtectionService::MiniRpc::MiniRpcClient>));
  EXPECT_FALSE((std::is_copy_constructible_v<
                OHOS::Security::VirusProtectionService::MemRpc::RpcClient>));
}

TEST(MiniRpcHeadersTest, ProtocolHeaderCompiles) {
  EXPECT_TRUE((std::is_enum_v<MiniRpcOpcode>));
  EXPECT_EQ(static_cast<uint16_t>(MiniRpcOpcode::MiniEcho), 200u);
}
