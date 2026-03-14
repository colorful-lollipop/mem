#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_

#include "memrpc/server/rpc_server.h"

namespace VirusExecutorService {

class RpcHandlerRegistrar {
 public:
    virtual ~RpcHandlerRegistrar() = default;
    virtual void RegisterHandlers(MemRpc::RpcServer* server) = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
