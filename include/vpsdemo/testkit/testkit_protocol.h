#ifndef INCLUDE_VPSDEMO_TESTKIT_TESTKIT_PROTOCOL_H_
#define INCLUDE_VPSDEMO_TESTKIT_TESTKIT_PROTOCOL_H_

#include <cstdint>

namespace vpsdemo::testkit {

enum class TestkitOpcode : uint16_t {
    Echo = 200,
    Add = 201,
    Sleep = 202,
    CrashForTest = 203,
    HangForTest = 204,
    OomForTest = 205,
    StackOverflowForTest = 206,
};

}  // namespace vpsdemo::testkit

#endif  // INCLUDE_VPSDEMO_TESTKIT_TESTKIT_PROTOCOL_H_
