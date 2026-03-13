#ifndef VPSDEMO_TESTKIT_STRESS_CONFIG_H_
#define VPSDEMO_TESTKIT_STRESS_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/protocol.h"

namespace vpsdemo::testkit {

struct StressConfig {
    int durationSec = 60;
    int warmupSec = 5;
    int threads = 1;
    int echoWeight = 70;
    int addWeight = 20;
    int sleepWeight = 10;
    int maxSleepMs = 5;
    int highPriorityPct = 20;
    int burstIntervalMs = 1000;
    int burstDurationMs = 200;
    int burstMultiplier = 3;
    int noProgressTimeoutSec = 30;
    uint32_t maxRequestBytes = memrpc::DEFAULT_MAX_REQUEST_BYTES;
    uint32_t maxResponseBytes = memrpc::DEFAULT_MAX_RESPONSE_BYTES;
    uint64_t seed = 0;
    std::vector<std::size_t> payloadSizes;
};

StressConfig ParseStressConfigFromEnv();

}  // namespace vpsdemo::testkit

#endif  // VPSDEMO_TESTKIT_STRESS_CONFIG_H_
