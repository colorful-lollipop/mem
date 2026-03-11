#ifndef APPS_MINIRPC_PROTOCOL_H_
#define APPS_MINIRPC_PROTOCOL_H_

#include <cstdint>

namespace OHOS::Security::VirusProtectionService::MiniRpc {

enum class MiniRpcOpcode : uint16_t {
    MiniEcho = 200,
    MiniAdd = 201,
    MiniSleep = 202,
    MiniCrashForTest = 203,
    MiniHangForTest = 204,
    MiniOomForTest = 205,
    MiniStackOverflowForTest = 206,
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PROTOCOL_H_
