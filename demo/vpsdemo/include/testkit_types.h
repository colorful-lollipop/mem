#ifndef VPSDEMO_TESTKIT_TYPES_H_
#define VPSDEMO_TESTKIT_TYPES_H_

#include <cstdint>
#include <string>

namespace vpsdemo::testkit {

struct EchoRequest {
    std::string text;
};

struct EchoReply {
    std::string text;
};

struct AddRequest {
    int32_t lhs = 0;
    int32_t rhs = 0;
};

struct AddReply {
    int32_t sum = 0;
};

struct SleepRequest {
    uint32_t delayMs = 0;
};

struct SleepReply {
    int32_t status = 0;
};

}  // namespace vpsdemo::testkit

#endif  // VPSDEMO_TESTKIT_TYPES_H_
