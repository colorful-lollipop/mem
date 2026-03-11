#ifndef VPSDEMO_VPSDEMO_TYPES_H_
#define VPSDEMO_VPSDEMO_TYPES_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

struct InitReply {
    int32_t code = 0;
};

struct ScanFileRequest {
    std::string file_path;
};

struct ScanFileReply {
    int32_t code = 0;
    int32_t threat_level = 0;  // 0 = clean, 1 = infected
};

struct UpdateFeatureLibReply {
    int32_t code = 0;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_TYPES_H_
