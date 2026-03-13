#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "testkit_stress_config.h"

namespace virus_executor_service::testkit {
namespace {

class ScopedEnv {
 public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        const char* previous = std::getenv(key);
        if (previous != nullptr) {
            hadPrevious_ = true;
            previousValue_ = previous;
        }
        setenv(key, value, 1);
    }

    ~ScopedEnv() {
        if (hadPrevious_) {
            setenv(key_, previousValue_.c_str(), 1);
        } else {
            unsetenv(key_);
        }
    }

 private:
    const char* key_;
    bool hadPrevious_ = false;
    std::string previousValue_;
};

}  // namespace

TEST(TestkitStressConfigTest, ParsesEnvOverrides) {
    ScopedEnv duration("MEMRPC_STRESS_DURATION_SEC", "5");
    ScopedEnv threads("MEMRPC_STRESS_THREADS", "3");
    ScopedEnv payloads("MEMRPC_STRESS_PAYLOAD_SIZES", "0,16,128");
    ScopedEnv highPriority("MEMRPC_STRESS_HIGH_PRIORITY_PCT", "25");
    ScopedEnv burst("MEMRPC_STRESS_BURST_INTERVAL_MS", "500");

    const StressConfig config = ParseStressConfigFromEnv();

    EXPECT_EQ(config.durationSec, 5);
    EXPECT_EQ(config.threads, 3);
    ASSERT_EQ(config.payloadSizes.size(), 3u);
    EXPECT_EQ(config.payloadSizes[0], 0u);
    EXPECT_EQ(config.payloadSizes[1], 16u);
    EXPECT_EQ(config.payloadSizes[2], 128u);
    EXPECT_EQ(config.highPriorityPct, 25);
    EXPECT_EQ(config.burstIntervalMs, 500);
}

}  // namespace virus_executor_service::testkit
