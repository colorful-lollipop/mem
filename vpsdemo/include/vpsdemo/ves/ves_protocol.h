#ifndef INCLUDE_VPSDEMO_VES_VES_PROTOCOL_H_
#define INCLUDE_VPSDEMO_VES_VES_PROTOCOL_H_

#include <cstdint>

namespace vpsdemo {

enum class VesOpcode : uint16_t {
    ScanFile = 102,
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_VES_VES_PROTOCOL_H_
