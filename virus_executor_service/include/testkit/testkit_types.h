#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_TYPES_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_TYPES_H_

#include <cstdint>
#include <string>

namespace VirusExecutorService::testkit {

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

}  // namespace VirusExecutorService::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_TYPES_H_
