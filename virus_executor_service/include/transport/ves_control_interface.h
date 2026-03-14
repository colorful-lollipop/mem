#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_

#include <cstdint>
#include <cstring>

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace VirusExecutorService {

constexpr int32_t VES_CONTROL_SA_ID = 1251;

enum class VesHeartbeatStatus : uint32_t {
    Ok = 0,
    Unhealthy = 1,
};

struct VesHeartbeatReply {
    uint32_t version = 1;
    uint32_t status = static_cast<uint32_t>(VesHeartbeatStatus::Unhealthy);
    uint64_t sessionId = 0;
    uint32_t inFlight = 0;
    uint32_t lastTaskAgeMs = 0;
    char currentTask[64] = {};
    uint32_t reserved[4] = {};
};

class IVesControl : public OHOS::IRemoteBroker {
 public:
    ~IVesControl() override = default;
    virtual MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) = 0;
    virtual MemRpc::StatusCode CloseSession() = 0;
    virtual MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
