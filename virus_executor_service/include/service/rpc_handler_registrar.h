#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_

#include "memrpc/server/rpc_server.h"

namespace VirusExecutorService {

class AnyCallHandlerSink {
 public:
    virtual ~AnyCallHandlerSink() = default;
    virtual void RegisterHandler(MemRpc::Opcode opcode, MemRpc::RpcHandler handler) = 0;
};

class RpcHandlerRegistrar {
 public:
    virtual ~RpcHandlerRegistrar() = default;
    virtual void RegisterHandlers(AnyCallHandlerSink* sink) = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
