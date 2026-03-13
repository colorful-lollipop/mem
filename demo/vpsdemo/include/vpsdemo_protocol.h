#ifndef VPSDEMO_VPSDEMO_PROTOCOL_H_
#define VPSDEMO_VPSDEMO_PROTOCOL_H_

#include <cstdint>

namespace vpsdemo {

enum class DemoOpcode : uint16_t {
    ScanFile = 102,
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_PROTOCOL_H_
