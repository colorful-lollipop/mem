#ifndef MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_
#define MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_

#include <cstdint>
#include <string>

#include "memrpc/core/protocol.h"

namespace MemRpc {

struct SharedMemorySessionConfig {
    uint32_t highRingSize = DEFAULT_HIGH_RING_SIZE;
    uint32_t normalRingSize = DEFAULT_NORMAL_RING_SIZE;
    uint32_t responseRingSize = DEFAULT_RESPONSE_RING_SIZE;
    uint32_t maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
    uint32_t maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;
    std::string shmName;
};

}  // namespace MemRpc

#endif  // MEMRPC_CORE_SHARED_MEMORY_SESSION_CONFIG_H_
