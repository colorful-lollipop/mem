#ifndef MEMRPC_SERVER_TYPED_HANDLER_H_
#define MEMRPC_SERVER_TYPED_HANDLER_H_

#include "memrpc/core/codec.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc {

// Convenience wrapper that auto-decodes the request and encodes the reply,
// so applications only need to provide a function: Req -> Rep.
template <typename Req, typename Rep, typename Handler>
void RegisterTypedHandler(RpcServer* server, Opcode opcode, Handler handler)
{
    server->RegisterHandler(opcode, [h = std::move(handler)](const RpcServerCall& call, RpcServerReply* reply) {
        if (reply == nullptr) {
            return;
        }
        Req request;
        if (!DecodeMessage<Req>(call.payload, &request)) {
            reply->status = StatusCode::ProtocolMismatch;
            return;
        }
        EncodeMessage<Rep>(h(request), &reply->payload);
    });
}

}  // namespace MemRpc

#endif  // MEMRPC_SERVER_TYPED_HANDLER_H_
