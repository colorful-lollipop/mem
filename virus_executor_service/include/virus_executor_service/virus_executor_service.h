#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_

#include <memory>

#include "system_ability.h"
#include "virus_executor_service/testkit/testkit_service.h"
#include "virus_executor_service/transport/ves_bootstrap_stub.h"
#include "virus_executor_service/ves/ves_engine_service.h"
#include "virus_executor_service/ves/ves_session_service.h"

namespace virus_executor_service {

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

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_
