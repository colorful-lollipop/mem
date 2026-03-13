#ifndef VPSDEMO_VES_BOOTSTRAP_INTERFACE_H_
#define VPSDEMO_VES_BOOTSTRAP_INTERFACE_H_

#include <cstdint>
#include <cstring>

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace vpsdemo {

// SA ID for the VPS bootstrap service.
constexpr int32_t VPS_BOOTSTRAP_SA_ID = 1251;

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

// Interface exposed by the engine SA for session establishment.
class IVesBootstrap : public OHOS::IRemoteBroker {
 public:
    ~IVesBootstrap() override = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
    virtual memrpc::StatusCode Heartbeat(VesHeartbeatReply& reply) = 0;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_BOOTSTRAP_INTERFACE_H_
