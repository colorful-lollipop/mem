#include <gtest/gtest.h>

#include "virus_protection_service_log.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

TEST(LogTest, NormalizeHilogVisibilityMarkers) {
  EXPECT_EQ(MemRpc::NormalizeLogFormat("value=%{public}s code=%{private}d done"),
            "value=%s code=%d done");
}
