#ifndef VPSDEMO_VPSDEMO_SAMPLE_RULES_H_
#define VPSDEMO_VPSDEMO_SAMPLE_RULES_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

struct SampleBehavior {
    int threatLevel = 0;
    uint32_t sleepMs = 0;
    bool shouldCrash = false;
};

// Evaluate a file path to determine scan behavior.
// Shared between server (VpsDemoService) and stress client for deterministic validation.
SampleBehavior EvaluateSamplePath(const std::string& path);

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_SAMPLE_RULES_H_
