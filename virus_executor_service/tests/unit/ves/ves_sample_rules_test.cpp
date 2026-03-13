#include <gtest/gtest.h>

#include "ves/ves_sample_rules.h"

namespace virus_executor_service {

TEST(VesSampleRulesTest, DetectsCrashKeyword) {
    auto behavior = EvaluateSamplePath("/data/crash_sample.apk");
    EXPECT_TRUE(behavior.shouldCrash);
}

TEST(VesSampleRulesTest, DetectsVirusKeywords) {
    EXPECT_EQ(EvaluateSamplePath("/data/virus.apk").threatLevel, 1);
    EXPECT_EQ(EvaluateSamplePath("/data/eicar.txt").threatLevel, 1);
}

TEST(VesSampleRulesTest, SleepSamplesAreThreats) {
    auto behavior = EvaluateSamplePath("/data/sleep10.bin");
    EXPECT_EQ(behavior.threatLevel, 1);
    EXPECT_EQ(behavior.sleepMs, 10u);
}

TEST(VesSampleRulesTest, SleepParsingRejectsNonDigit) {
    auto behavior = EvaluateSamplePath("/data/sleepX.bin");
    EXPECT_EQ(behavior.sleepMs, 0u);
}

TEST(VesSampleRulesTest, SleepCappedAtMax) {
    auto behavior = EvaluateSamplePath("/data/sleep999999.bin");
    EXPECT_EQ(behavior.sleepMs, 5000u);
}

}  // namespace virus_executor_service
