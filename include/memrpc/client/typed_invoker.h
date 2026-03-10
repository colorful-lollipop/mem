#ifndef MEMRPC_CLIENT_TYPED_INVOKER_H_
#define MEMRPC_CLIENT_TYPED_INVOKER_H_

#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/codec.h"

namespace memrpc {

// Encode request, build RpcCall, and invoke asynchronously.
template <typename Req>
RpcFuture InvokeTyped(RpcClient* client,
                      Opcode opcode,
                      const Req& request,
                      Priority priority = Priority::Normal,
                      uint32_t exec_timeout_ms = 30000) {
    std::vector<uint8_t> payload;
    if (!EncodeMessage<Req>(request, &payload)) {
        return {};
    }
    RpcCall call;
    call.opcode = opcode;
    call.priority = priority;
    call.exec_timeout_ms = exec_timeout_ms;
    call.payload = std::move(payload);
    return client->InvokeAsync(std::move(call));
}

// Wait on a future and decode the reply payload.
template <typename Rep>
StatusCode WaitAndDecode(RpcFuture future, Rep* reply) {
    if (reply == nullptr) {
        return StatusCode::InvalidArgument;
    }
    RpcReply rpcReply;
    const StatusCode status = future.WaitAndTake(&rpcReply);
    if (status != StatusCode::Ok) {
        return status;
    }
    return DecodeMessage<Rep>(rpcReply.payload, reply) ? rpcReply.status
                                                       : StatusCode::ProtocolMismatch;
}

// Register a callback that decodes the reply payload before invoking.
// Callback signature: void(StatusCode, Rep).
template <typename Rep>
void Then(RpcFuture future,
          std::function<void(StatusCode, Rep)> callback,
          RpcThenExecutor executor = {}) {
    future.Then([cb = std::move(callback)](RpcReply rpcReply) {
        if (rpcReply.status != StatusCode::Ok) {
            cb(rpcReply.status, {});
            return;
        }
        Rep decoded{};
        if (!DecodeMessage<Rep>(rpcReply.payload, &decoded)) {
            cb(StatusCode::ProtocolMismatch, {});
            return;
        }
        cb(rpcReply.status, std::move(decoded));
    }, std::move(executor));
}

// Synchronous convenience: encode, invoke, wait, decode — all in one call.
template <typename Req, typename Rep>
StatusCode InvokeTypedSync(RpcClient* client,
                           Opcode opcode,
                           const Req& request,
                           Rep* reply,
                           Priority priority = Priority::Normal,
                           uint32_t exec_timeout_ms = 30000) {
    return WaitAndDecode<Rep>(
        InvokeTyped<Req>(client, opcode, request, priority, exec_timeout_ms), reply);
}

}  // namespace memrpc

#endif  // MEMRPC_CLIENT_TYPED_INVOKER_H_
