#ifndef VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_
#define VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_

#include <cstdint>
#include <cstring>

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace vpsdemo {

// SA ID for the VPS bootstrap service.
constexpr int32_t VPS_BOOTSTRAP_SA_ID = 1251;

enum class VpsHeartbeatStatus : uint32_t {
    Ok = 0,
    Unhealthy = 1,
};

struct VpsHeartbeatReply {
    uint32_t version = 1;
    uint32_t status = static_cast<uint32_t>(VpsHeartbeatStatus::Unhealthy);
    uint64_t session_id = 0;
    uint32_t in_flight = 0;
    uint32_t last_task_age_ms = 0;
    char current_task[64] = {};
    uint32_t reserved[4] = {};
};

// Interface exposed by the engine SA for session establishment.
class IVpsBootstrap : public OHOS::IRemoteBroker {
 public:
    ~IVpsBootstrap() override = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
    virtual memrpc::StatusCode Heartbeat(VpsHeartbeatReply& reply) = 0;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_
