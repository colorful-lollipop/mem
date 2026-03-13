#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_

#include <cstdint>
#include <string>

namespace virus_executor_service {

struct InitReply {
    int32_t code = 0;
};

struct ScanFileRequest {
    std::string filePath;
};

struct ScanFileReply {
    int32_t code = 0;
    int32_t threatLevel = 0;
};

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_TYPES_H_
