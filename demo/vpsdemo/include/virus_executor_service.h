#ifndef VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
#define VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_

#include <memory>

#include "system_ability.h"
#include "vps_bootstrap_stub.h"
#include "ves_session_service.h"
#include "ves_engine_service.h"

namespace vpsdemo {

// Engine-side SA: pure business logic.
// Transport is managed by the framework (SystemAbility::Publish auto-starts MockServiceSocket).
// Command dispatch is handled by VesBootstrapStub (OnRemoteRequest).
class VirusExecutorService : public OHOS::SystemAbility,
                              public VesBootstrapStub {
 public:
    VirusExecutorService();
    ~VirusExecutorService() override = default;

    // IVesBootstrap — delegates to session_service_.
    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;
    memrpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;

    // SystemAbility lifecycle.
    void OnStart() override;
    void OnStop() override;

    VesEngineService& service() { return service_; }

 private:
    VesEngineService service_;
    std::shared_ptr<EngineSessionService> session_service_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
