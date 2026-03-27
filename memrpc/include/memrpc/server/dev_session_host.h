#ifndef MEMRPC_SERVER_DEV_SESSION_HOST_H_
#define MEMRPC_SERVER_DEV_SESSION_HOST_H_

#include <memory>

#include "memrpc/core/bootstrap.h"
#include "memrpc/core/dev_bootstrap_config.h"

namespace MemRpc {

class IServerSessionHost {
public:
    virtual ~IServerSessionHost() = default;

    virtual StatusCode EnsureSession() = 0;
    virtual StatusCode OpenSession(BootstrapHandles& handles) = 0;
    virtual StatusCode CloseSession() = 0;
    [[nodiscard]] virtual BootstrapHandles serverHandles() const = 0;
};

class DevSessionHost final : public IServerSessionHost {
public:
    explicit DevSessionHost(DevBootstrapConfig config = {});
    ~DevSessionHost() override;

    StatusCode EnsureSession() override;
    StatusCode OpenSession(BootstrapHandles& handles) override;
    StatusCode CloseSession() override;
    [[nodiscard]] BootstrapHandles serverHandles() const override;
    [[nodiscard]] uint64_t sessionId() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_SERVER_DEV_SESSION_HOST_H_
