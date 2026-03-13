#ifndef VPSDEMO_VES_PROTOCOL_H_
#define VPSDEMO_VES_PROTOCOL_H_

#include <cstdint>

namespace vpsdemo {

enum class VesOpcode : uint16_t {
    ScanFile = 102,
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_PROTOCOL_H_
