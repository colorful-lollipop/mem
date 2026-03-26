#ifndef MEMRPC_CLIENT_TYPED_INVOKER_H_
#define MEMRPC_CLIENT_TYPED_INVOKER_H_

#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/codec.h"

namespace MemRpc {

// Wait on a future and decode the reply payload.
template <typename Rep>
StatusCode WaitAndDecode(RpcFuture&& future, Rep* reply)
{
    if (reply == nullptr) {
        return StatusCode::InvalidArgument;
    }
    RpcReply rpcReply;
    const StatusCode status = std::move(future).Wait(&rpcReply);
    if (status != StatusCode::Ok) {
        return status;
    }
    return DecodeMessage<Rep>(rpcReply.payload, reply) ? rpcReply.status : StatusCode::ProtocolMismatch;
}

}  // namespace MemRpc

#endif  // MEMRPC_CLIENT_TYPED_INVOKER_H_
