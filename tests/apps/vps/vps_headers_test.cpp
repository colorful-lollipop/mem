#include <gtest/gtest.h>

#include <type_traits>

#include "apps/vps/protocol.h"

using OHOS::Security::VirusProtectionService::VpsOpcode;

TEST(VpsHeadersTest, ProtocolHeaderCompiles) {
    EXPECT_TRUE((std::is_enum_v<VpsOpcode>));
    EXPECT_EQ(static_cast<uint16_t>(VpsOpcode::VpsInit), 100u);
}
