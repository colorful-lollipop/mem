#ifndef MEMRPC_CLIENT_DEV_BOOTSTRAP_H_
#define MEMRPC_CLIENT_DEV_BOOTSTRAP_H_

#include <memory>

#include "memrpc/core/bootstrap.h"
#include "memrpc/core/dev_bootstrap_config.h"

namespace MemRpc {

class DevBootstrapChannel : public IBootstrapChannel {
public:
    explicit DevBootstrapChannel(DevBootstrapConfig config = {});
    ~DevBootstrapChannel() override;

    StatusCode OpenSession(BootstrapHandles& handles) override;
    StatusCode CloseSession() override;
    ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
    void SetEngineDeathCallback(EngineDeathCallback callback) override;

    [[nodiscard]] BootstrapHandles serverHandles() const;
    void SimulateEngineDeathForTest(uint64_t session_id = 0);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CLIENT_DEV_BOOTSTRAP_H_
