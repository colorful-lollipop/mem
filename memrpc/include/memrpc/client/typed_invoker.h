#ifndef MEMRPC_CLIENT_TYPED_INVOKER_H_
#define MEMRPC_CLIENT_TYPED_INVOKER_H_

#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/client/typed_future.h"
#include "memrpc/core/codec.h"

namespace MemRpc {

struct TypedInvokeOptions {
    Priority priority = Priority::Normal;
    uint32_t execTimeoutMs = 30000;
};

// Encode request, build RpcCall, and invoke asynchronously.
template <typename Req>
RpcFuture InvokeTyped(RpcClient* client,
                      Opcode opcode,
                      const Req& request,
                      Priority priority = Priority::Normal,
                      uint32_t exec_timeout_ms = 30000)
{
    std::vector<uint8_t> payload;
    if (!EncodeMessage<Req>(request, &payload)) {
        return {};
    }
    RpcCall call;
    call.opcode = opcode;
    call.priority = priority;
    call.execTimeoutMs = exec_timeout_ms;
    call.payload = std::move(payload);
    return client->InvokeAsync(std::move(call));
}

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

// Encode request and invoke asynchronously, returning a TypedFuture<Rep> that
// performs owning decode at consumption time (Wait).
template <typename Req, typename Rep>
TypedFuture<Rep> InvokeTypedAsync(RpcClient* client,
                                  Opcode opcode,
                                  const Req& request,
                                  Priority priority = Priority::Normal,
                                  uint32_t exec_timeout_ms = 30000)
{
    return TypedFuture<Rep>(InvokeTyped<Req>(client, opcode, request, priority, exec_timeout_ms));
}

// Synchronous convenience: encode, invoke, wait, decode — all in one call.
template <typename Req, typename Rep>
StatusCode InvokeTypedSync(RpcClient* client,
                           Opcode opcode,
                           const Req& request,
                           Rep* reply,
                           TypedInvokeOptions options = {})
{
    return WaitAndDecode<Rep>(
        InvokeTyped<Req>(client, opcode, request, options.priority, options.execTimeoutMs), reply);
}

}  // namespace MemRpc

#endif  // MEMRPC_CLIENT_TYPED_INVOKER_H_
