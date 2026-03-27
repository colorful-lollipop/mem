#ifndef MEMRPC_CORE_DEV_BOOTSTRAP_CONFIG_H_
#define MEMRPC_CORE_DEV_BOOTSTRAP_CONFIG_H_

#include <cstdint>
#include <string>

#include "memrpc/core/protocol.h"

namespace MemRpc {

struct DevBootstrapConfig {
    uint32_t highRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.highRingSize;
    uint32_t normalRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.normalRingSize;
    uint32_t responseRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.responseRingSize;
    uint32_t maxRequestBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxRequestBytes;
    uint32_t maxResponseBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxResponseBytes;
    std::string shmName;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CORE_DEV_BOOTSTRAP_CONFIG_H_
