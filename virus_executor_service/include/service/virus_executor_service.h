#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_

#include <memory>

#include "system_ability.h"
#include "testkit/testkit_service.h"
#include "transport/ves_control_stub.h"
#include "ves/ves_engine_service.h"
#include "ves/ves_session_service.h"

namespace VirusExecutorService {

class VirusExecutorService : public OHOS::SystemAbility,
                              public VesControlStub {
 public:
    VirusExecutorService();
    ~VirusExecutorService() override = default;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;

    void OnStart() override;
    void OnStop() override;

    VesEngineService& service() { return service_; }

 private:
    VesEngineService service_;
    testkit::TestkitService testkitService_;
    std::shared_ptr<EngineSessionService> session_service_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VIRUS_EXECUTOR_SERVICE_H_
