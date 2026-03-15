#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_

#include <cstdint>
#include <string>

namespace VirusExecutorService {

struct InitReply {
    int32_t code = 0;
};

struct ScanTask {
    std::string path;
};

struct ScanFileReply {
    int32_t code = 0;
    int32_t threatLevel = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_
