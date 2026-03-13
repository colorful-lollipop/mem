#ifndef INCLUDE_VPSDEMO_VES_VES_TYPES_H_
#define INCLUDE_VPSDEMO_VES_VES_TYPES_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

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

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_VES_VES_TYPES_H_
