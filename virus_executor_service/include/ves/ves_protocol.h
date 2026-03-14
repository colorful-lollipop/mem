#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_

#include <cstdint>

namespace VirusExecutorService {

enum class VesOpcode : uint16_t {
    ScanFile = 102,
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
