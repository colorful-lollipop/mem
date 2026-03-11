#ifndef VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
#define VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_

#include <memory>

#include "iremote_stub.h"
#include "mock_service_socket.h"
#include "system_ability.h"
#include "vps_bootstrap_interface.h"
#include "vps_session_service.h"

namespace vpsdemo {

// Engine-side SA: delegates session management to an injected VpsSessionProvider.
// Mock IPC transport (Unix socket + SCM_RIGHTS) is handled by MockServiceSocket.
class VirusExecutorService : public OHOS::SystemAbility,
                              public OHOS::IRemoteStub<IVpsBootstrap> {
 public:
    explicit VirusExecutorService(std::shared_ptr<VpsSessionProvider> provider);
    ~VirusExecutorService() override;

    // IVpsBootstrap — delegates to provider.
    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) override;
    memrpc::StatusCode CloseSession() override;

    // SystemAbility lifecycle.
    void OnStart() override;
    void OnStop() override;

 private:
    std::shared_ptr<VpsSessionProvider> provider_;
    OHOS::MockServiceSocket transport_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
