#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_

#include <memory>
#include <mutex>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"

namespace VirusExecutorService {

class VesSessionProvider {
 public:
    virtual ~VesSessionProvider() = default;
    virtual MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) = 0;
    virtual MemRpc::StatusCode CloseSession() = 0;
};

class EngineSessionService final : public VesSessionProvider {
 public:
    explicit EngineSessionService(std::vector<RpcHandlerRegistrar*> registrars = {});

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;

    [[nodiscard]] uint64_t session_id() const;

 private:
    MemRpc::StatusCode EnsureInitialized();

    std::vector<RpcHandlerRegistrar*> registrars_;
    std::shared_ptr<MemRpc::DevBootstrapChannel> bootstrap_;
    std::unique_ptr<MemRpc::RpcServer> rpcServer_;
    std::mutex initMutex_;
    bool initialized_ = false;
    uint64_t sessionId_ = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_
