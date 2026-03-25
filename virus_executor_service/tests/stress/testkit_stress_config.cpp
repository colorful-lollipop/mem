#include "testkit_stress_config.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

namespace VirusExecutorService::testkit {
namespace {

int GetEnvInt(const char* name, int defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    try {
        const int parsed = std::stoi(value);
        return parsed >= 0 ? parsed : defaultValue;
    } catch (const std::exception&) {
        return defaultValue;
    }
}

uint64_t GetEnvUint64(const char* name, uint64_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (const std::exception&) {
        return defaultValue;
    }
}

std::vector<std::size_t> ParseCsvSizes(const char* value)
{
    std::vector<std::size_t> sizes;
    if (value == nullptr || *value == '\0') {
        return sizes;
    }
    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) {
            continue;
        }
        try {
            sizes.push_back(static_cast<std::size_t>(std::stoull(token)));
        } catch (const std::exception&) {
            continue;
        }
    }
    return sizes;
}

std::vector<std::size_t> DefaultPayloadSizes(uint32_t maxRequestBytes)
{
    const std::size_t safeMax = maxRequestBytes >= 64 ? maxRequestBytes - 64 : 0;
    std::vector<std::size_t> sizes = {0, 16, 128, 512, 1024, 2048};
    if (safeMax > 2048) {
        sizes.push_back(safeMax);
    }
    sizes.erase(std::remove_if(sizes.begin(), sizes.end(), [safeMax](std::size_t value) { return value > safeMax; }),
                sizes.end());
    if (sizes.empty()) {
        sizes.push_back(0);
    }
    return sizes;
}

void ClampPayloadSizes(std::vector<std::size_t>* sizes, uint32_t maxRequestBytes)
{
    if (sizes == nullptr) {
        return;
    }
    sizes->erase(std::remove_if(sizes->begin(),
                                sizes->end(),
                                [maxRequestBytes](std::size_t value) { return value > maxRequestBytes; }),
                 sizes->end());
    if (sizes->empty()) {
        sizes->push_back(0);
    }
}

}  // namespace

StressConfig ParseStressConfigFromEnv()
{
    StressConfig config;
    config.durationSec = GetEnvInt("MEMRPC_STRESS_DURATION_SEC", config.durationSec);
    config.warmupSec = GetEnvInt("MEMRPC_STRESS_WARMUP_SEC", config.warmupSec);
    config.threads = GetEnvInt("MEMRPC_STRESS_THREADS", config.threads);
    config.echoWeight = GetEnvInt("MEMRPC_STRESS_ECHO_WEIGHT", config.echoWeight);
    config.addWeight = GetEnvInt("MEMRPC_STRESS_ADD_WEIGHT", config.addWeight);
    config.sleepWeight = GetEnvInt("MEMRPC_STRESS_SLEEP_WEIGHT", config.sleepWeight);
    config.maxSleepMs = GetEnvInt("MEMRPC_STRESS_MAX_SLEEP_MS", config.maxSleepMs);
    config.highPriorityPct = GetEnvInt("MEMRPC_STRESS_HIGH_PRIORITY_PCT", config.highPriorityPct);
    config.burstIntervalMs = GetEnvInt("MEMRPC_STRESS_BURST_INTERVAL_MS", config.burstIntervalMs);
    config.burstDurationMs = GetEnvInt("MEMRPC_STRESS_BURST_durationMs", config.burstDurationMs);
    config.burstMultiplier = GetEnvInt("MEMRPC_STRESS_BURST_MULTIPLIER", config.burstMultiplier);
    config.noProgressTimeoutSec = GetEnvInt("MEMRPC_STRESS_NO_PROGRESS_SEC", config.noProgressTimeoutSec);
    config.maxRequestBytes =
        static_cast<uint32_t>(GetEnvInt("MEMRPC_STRESS_MAX_REQUEST_BYTES", static_cast<int>(config.maxRequestBytes)));
    config.maxResponseBytes =
        static_cast<uint32_t>(GetEnvInt("MEMRPC_STRESS_MAX_RESPONSE_BYTES", static_cast<int>(config.maxResponseBytes)));
    config.seed = GetEnvUint64("MEMRPC_STRESS_SEED", config.seed);

    const char* sizes = std::getenv("MEMRPC_STRESS_PAYLOAD_SIZES");
    if (sizes != nullptr && *sizes != '\0') {
        config.payloadSizes = ParseCsvSizes(sizes);
    } else {
        config.payloadSizes = DefaultPayloadSizes(config.maxRequestBytes);
    }
    ClampPayloadSizes(&config.payloadSizes, config.maxRequestBytes);

    if (config.threads <= 0) {
        config.threads = 1;
    }
    if (config.burstIntervalMs < 0) {
        config.burstIntervalMs = 0;
    }
    if (config.burstDurationMs < 0) {
        config.burstDurationMs = 0;
    }
    if (config.burstMultiplier < 1) {
        config.burstMultiplier = 1;
    }
    return config;
}

}  // namespace VirusExecutorService::testkit
