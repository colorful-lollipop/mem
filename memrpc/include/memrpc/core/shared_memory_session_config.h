#ifndef MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_
#define MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_

#include <cstdint>
#include <string>

#include "memrpc/core/protocol.h"

namespace MemRpc {

struct SharedMemorySessionConfig {
    uint32_t highRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.highRingSize;
    uint32_t normalRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.normalRingSize;
    uint32_t responseRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.responseRingSize;
    uint32_t maxRequestBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxRequestBytes;
    uint32_t maxResponseBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxResponseBytes;
    std::string shmName;
};

}  // namespace MemRpc

#endif  // MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_
