#include "vesdemo_sample_rules.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace vpsdemo {
namespace {

constexpr uint32_t MAX_SLEEP_MS = 5000;

uint32_t ParseSleepMs(const std::string& path) {
    const std::string key = "sleep";
    auto pos = path.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    if (pos >= path.size() || !std::isdigit(static_cast<unsigned char>(path[pos]))) return 0;
    uint32_t ms = 0;
    while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos]))) {
        ms = ms * 10 + static_cast<uint32_t>(path[pos] - '0');
        pos++;
    }
    return std::min(ms, MAX_SLEEP_MS);
}

}  // namespace

SampleBehavior EvaluateSamplePath(const std::string& path) {
    SampleBehavior behavior;
    if (path.find("crash") != std::string::npos) {
        behavior.shouldCrash = true;
    }
    behavior.sleepMs = ParseSleepMs(path);
    if (path.find("virus") != std::string::npos ||
        path.find("eicar") != std::string::npos) {
        behavior.threatLevel = 1;
    }
    // sleep samples are also considered threats
    if (behavior.sleepMs > 0) {
        behavior.threatLevel = 1;
    }
    return behavior;
}

}  // namespace vpsdemo
