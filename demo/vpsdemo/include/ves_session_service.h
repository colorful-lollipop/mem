#ifndef VPSDEMO_VES_SESSION_SERVICE_H_
#define VPSDEMO_VES_SESSION_SERVICE_H_

#include <memory>
#include <mutex>
#include <vector>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/rpc_server.h"
#include "rpc_handler_registrar.h"

namespace vpsdemo {

class VesSessionProvider {
 public:
    virtual ~VesSessionProvider() = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
};

class EngineSessionService final : public VesSessionProvider {
 public:
    explicit EngineSessionService(std::vector<RpcHandlerRegistrar*> registrars = {});

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;

    uint64_t session_id() const;

 private:
    memrpc::StatusCode EnsureInitialized();

    std::vector<RpcHandlerRegistrar*> registrars_;
    std::shared_ptr<memrpc::PosixDemoBootstrapChannel> bootstrap_;
    std::unique_ptr<memrpc::RpcServer> rpcServer_;
    std::mutex initMutex_;
    bool initialized_ = false;
    uint64_t sessionId_ = 0;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_SESSION_SERVICE_H_
