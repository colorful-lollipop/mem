#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_

#include <cstdint>

namespace virus_executor_service {

enum class VesOpcode : uint16_t {
    ScanFile = 102,
};

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_PROTOCOL_H_
