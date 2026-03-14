#ifndef VIRUS_EXECUTOR_SERVICE_TESTKIT_STRESS_CONFIG_H_
#define VIRUS_EXECUTOR_SERVICE_TESTKIT_STRESS_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memrpc/core/protocol.h"

namespace VirusExecutorService::testkit {

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
    uint32_t maxRequestBytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES;
    uint32_t maxResponseBytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES;
    uint64_t seed = 0;
    std::vector<std::size_t> payloadSizes;
};

StressConfig ParseStressConfigFromEnv();

}  // namespace VirusExecutorService::testkit

#endif  // VIRUS_EXECUTOR_SERVICE_TESTKIT_STRESS_CONFIG_H_
