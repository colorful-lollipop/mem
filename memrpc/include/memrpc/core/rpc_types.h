#ifndef MEMRPC_CORE_RPC_TYPES_H_
#define MEMRPC_CORE_RPC_TYPES_H_

#include <vector>

#include "memrpc/core/types.h"

namespace MemRpc {

struct RpcReply {
    // status 是框架层结果；业务错误需要放进应用层 payload。
    StatusCode status = StatusCode::Ok;
    std::vector<uint8_t> payload;
};

}  // namespace MemRpc

#endif  // MEMRPC_CORE_RPC_TYPES_H_
