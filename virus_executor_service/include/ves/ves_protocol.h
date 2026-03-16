#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_

#include <cstdint>

namespace VirusExecutorService {

enum class VesOpcode : uint8_t {
    ScanFile = 102,
};

inline constexpr uint32_t VES_EVENT_DOMAIN_RUNTIME = 1;

enum class VesEventType : uint32_t {
    RandomScanResult = 1,
    RandomHealthHint = 2,
    RandomLifecycle = 3,
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
