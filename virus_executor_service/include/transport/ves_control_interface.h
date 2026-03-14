#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_

#include <cstdint>
#include <cstring>
#include <vector>

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace VirusExecutorService {

constexpr int32_t VES_CONTROL_SA_ID = 1251;

enum class VesHeartbeatStatus : uint32_t {
    OkIdle = 0,
    OkBusy = 1,
    DegradedLongRunning = 2,
    UnhealthyNoSession = 3,
    UnhealthyStopping = 4,
    UnhealthyInternalError = 5,
};

enum class VesHeartbeatReasonCode : uint32_t {
    None = 0,
    Busy = 1,
    LongRunning = 2,
    NoSession = 3,
    Stopping = 4,
    InternalError = 5,
};

constexpr uint32_t VES_HEARTBEAT_FLAG_HAS_SESSION = 1u << 0;
constexpr uint32_t VES_HEARTBEAT_FLAG_INITIALIZED = 1u << 1;
constexpr uint32_t VES_HEARTBEAT_FLAG_BUSY = 1u << 2;
constexpr uint32_t VES_HEARTBEAT_FLAG_LONG_RUNNING = 1u << 3;

struct VesHeartbeatReply {
    uint32_t version = 2;
    uint32_t status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession);
    uint32_t reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession);
    uint64_t sessionId = 0;
    uint32_t inFlight = 0;
    uint32_t lastTaskAgeMs = 0;
    char currentTask[64] = {};
    uint32_t flags = 0;
    uint32_t reserved[3] = {};
};

struct VesAnyCallRequest {
    uint16_t opcode = 0;
    uint16_t reserved = 0;
    uint32_t flags = 0;
    uint32_t timeoutMs = 0;
    std::vector<uint8_t> payload;
};

struct VesAnyCallReply {
    MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
    int32_t errorCode = 0;
    std::vector<uint8_t> payload;
};

class IVesControl : public OHOS::IRemoteBroker {
 public:
    ~IVesControl() override = default;
    virtual MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) = 0;
    virtual MemRpc::StatusCode CloseSession() = 0;
    virtual MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) = 0;
    virtual MemRpc::StatusCode AnyCall(const VesAnyCallRequest& request,
                                       VesAnyCallReply& reply) = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
