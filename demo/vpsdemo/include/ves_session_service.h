#ifndef VPSDEMO_VES_SESSION_SERVICE_H_
#define VPSDEMO_VES_SESSION_SERVICE_H_

#include <memory>
#include <mutex>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/rpc_server.h"

namespace vpsdemo {

class VpsDemoService;

class VesSessionProvider {
 public:
    virtual ~VesSessionProvider() = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
};

class EngineSessionService final : public VesSessionProvider {
 public:
    explicit EngineSessionService(VpsDemoService* service);

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;

 private:
    memrpc::StatusCode EnsureInitialized();

    VpsDemoService* service_ = nullptr;
    std::shared_ptr<memrpc::PosixDemoBootstrapChannel> bootstrap_;
    std::unique_ptr<memrpc::RpcServer> rpc_server_;
    std::mutex init_mutex_;
    bool initialized_ = false;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_SESSION_SERVICE_H_
