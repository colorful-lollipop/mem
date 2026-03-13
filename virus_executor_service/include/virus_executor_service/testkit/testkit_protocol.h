#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_PROTOCOL_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_PROTOCOL_H_

#include <cstdint>

namespace virus_executor_service::testkit {

enum class TestkitOpcode : uint16_t {
    Echo = 200,
    Add = 201,
    Sleep = 202,
    CrashForTest = 203,
    HangForTest = 204,
    OomForTest = 205,
    StackOverflowForTest = 206,
};

}  // namespace virus_executor_service::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_PROTOCOL_H_
