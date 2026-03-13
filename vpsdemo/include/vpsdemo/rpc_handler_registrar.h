#ifndef INCLUDE_VPSDEMO_RPC_HANDLER_REGISTRAR_H_
#define INCLUDE_VPSDEMO_RPC_HANDLER_REGISTRAR_H_

#include "memrpc/server/rpc_server.h"

namespace vpsdemo {

class RpcHandlerRegistrar {
 public:
    virtual ~RpcHandlerRegistrar() = default;
    virtual void RegisterHandlers(memrpc::RpcServer* server) = 0;
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_RPC_HANDLER_REGISTRAR_H_
