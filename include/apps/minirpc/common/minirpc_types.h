#ifndef APPS_MINIRPC_COMMON_MINIRPC_TYPES_H_
#define APPS_MINIRPC_COMMON_MINIRPC_TYPES_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace OHOS::Security::VirusProtectionService::MiniRpc {

struct EchoRequest {
  std::string text;
};

struct EchoRequestView {
  std::string_view text;
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
  uint32_t delay_ms = 0;
};

struct SleepReply {
  int32_t status = 0;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_COMMON_MINIRPC_TYPES_H_
