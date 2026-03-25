#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_

#include <utility>

#include "memrpc/core/codec.h"
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
        : server_(server)
    {
    }

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

template <typename Request, typename Reply, typename Handler>
inline void RegisterTypedHandler(RpcHandlerSink* sink, MemRpc::Opcode opcode, Handler handler)
{
    if (sink == nullptr) {
        return;
    }

    sink->RegisterHandler(opcode,
                          [h = std::move(handler)](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                              if (reply == nullptr) {
                                  return;
                              }

                              Request request;
                              if (!MemRpc::DecodeMessage<Request>(call.payload, &request)) {
                                  reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                  return;
                              }

                              if (!MemRpc::EncodeMessage<Reply>(h(request), &reply->payload)) {
                                  reply->status = MemRpc::StatusCode::EngineInternalError;
                                  reply->payload.clear();
                              }
                          });
}

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_RPC_HANDLER_REGISTRAR_H_
