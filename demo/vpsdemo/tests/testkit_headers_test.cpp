#include <gtest/gtest.h>

#include <type_traits>

#include "memrpc/client/rpc_client.h"
#include "testkit_client.h"
#include "testkit_protocol.h"
#include "testkit_types.h"

namespace vpsdemo::testkit {

TEST(TestkitHeadersTest, HeaderLayoutComposes) {
    EXPECT_TRUE((std::is_default_constructible_v<EchoRequest>));
    EXPECT_TRUE((std::is_class_v<TestkitClient>));
    EXPECT_FALSE((std::is_copy_constructible_v<memrpc::RpcClient>));
}

TEST(TestkitHeadersTest, ProtocolHeaderCompiles) {
    EXPECT_TRUE((std::is_enum_v<TestkitOpcode>));
    EXPECT_EQ(static_cast<uint16_t>(TestkitOpcode::Echo), 200u);
    EXPECT_EQ(static_cast<uint16_t>(TestkitOpcode::CrashForTest), 203u);
}

}  // namespace vpsdemo::testkit
