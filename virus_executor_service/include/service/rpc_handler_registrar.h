#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_

#include <utility>

#include "memrpc/server/rpc_server.h"

namespace VirusExecutorService {

class RpcHandlerSink {
 public:
    virtual ~RpcHandlerSink() = default;
    virtual void RegisterHandler(MemRpc::Opcode opcode, MemRpc::RpcHandler handler) = 0;
};

class RpcServerHandlerSink final : public RpcHandlerSink {
 public:
    explicit RpcServerHandlerSink(MemRpc::RpcServer* server)
        : server_(server) {}

    void RegisterHandler(MemRpc::Opcode opcode, MemRpc::RpcHandler handler) override
    {
        if (server_ == nullptr) {
            return;
        }
        server_->RegisterHandler(opcode, std::move(handler));
    }

 private:
    MemRpc::RpcServer* server_ = nullptr;
};

class RpcHandlerRegistrar {
 public:
    virtual ~RpcHandlerRegistrar() = default;
    virtual void RegisterHandlers(RpcHandlerSink* sink) = 0;
};

inline void RegisterHandlersToServer(RpcHandlerRegistrar* registrar, MemRpc::RpcServer* server)
{
    if (registrar == nullptr) {
        return;
    }
    RpcServerHandlerSink sink(server);
    registrar->RegisterHandlers(&sink);
}

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
