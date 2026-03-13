#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VESDEMO_SAMPLE_RULES_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VESDEMO_SAMPLE_RULES_H_

#include <cstdint>
#include <string>

namespace virus_executor_service {

struct SampleBehavior {
    int threatLevel = 0;
    uint32_t sleepMs = 0;
    bool shouldCrash = false;
};

SampleBehavior EvaluateSamplePath(const std::string& path);

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VESDEMO_SAMPLE_RULES_H_
