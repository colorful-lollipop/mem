#include <gtest/gtest.h>

#include "vpsdemo_sample_rules.h"

namespace vpsdemo {

TEST(VpsSampleRulesTest, DetectsCrashKeyword) {
    auto behavior = EvaluateSamplePath("/data/crash_sample.apk");
    EXPECT_TRUE(behavior.shouldCrash);
}

TEST(VpsSampleRulesTest, DetectsVirusKeywords) {
    EXPECT_EQ(EvaluateSamplePath("/data/virus.apk").threatLevel, 1);
    EXPECT_EQ(EvaluateSamplePath("/data/eicar.txt").threatLevel, 1);
}

TEST(VpsSampleRulesTest, SleepSamplesAreThreats) {
    auto behavior = EvaluateSamplePath("/data/sleep10.bin");
    EXPECT_EQ(behavior.threatLevel, 1);
    EXPECT_EQ(behavior.sleepMs, 10u);
}

TEST(VpsSampleRulesTest, SleepParsingRejectsNonDigit) {
    auto behavior = EvaluateSamplePath("/data/sleepX.bin");
    EXPECT_EQ(behavior.sleepMs, 0u);
}

TEST(VpsSampleRulesTest, SleepCappedAtMax) {
    auto behavior = EvaluateSamplePath("/data/sleep999999.bin");
    EXPECT_EQ(behavior.sleepMs, 5000u);
}

}  // namespace vpsdemo
