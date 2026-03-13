#ifndef INCLUDE_VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
#define INCLUDE_VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_

#include <memory>

#include "system_ability.h"
#include "vpsdemo/testkit/testkit_service.h"
#include "vpsdemo/transport/ves_bootstrap_stub.h"
#include "vpsdemo/ves/ves_engine_service.h"
#include "vpsdemo/ves/ves_session_service.h"

namespace vpsdemo {

class VirusExecutorService : public OHOS::SystemAbility,
                              public VesBootstrapStub {
 public:
    VirusExecutorService();
    ~VirusExecutorService() override = default;

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;
    memrpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;

    void OnStart() override;
    void OnStop() override;

    VesEngineService& service() { return service_; }

 private:
    VesEngineService service_;
    testkit::TestkitService testkitService_;
    std::shared_ptr<EngineSessionService> session_service_;
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_VIRUS_EXECUTOR_SERVICE_H_
