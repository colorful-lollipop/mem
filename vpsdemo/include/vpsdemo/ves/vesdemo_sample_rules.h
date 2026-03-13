#ifndef INCLUDE_VPSDEMO_VES_VESDEMO_SAMPLE_RULES_H_
#define INCLUDE_VPSDEMO_VES_VESDEMO_SAMPLE_RULES_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

struct SampleBehavior {
    int threatLevel = 0;
    uint32_t sleepMs = 0;
    bool shouldCrash = false;
};

SampleBehavior EvaluateSamplePath(const std::string& path);

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_VES_VESDEMO_SAMPLE_RULES_H_
