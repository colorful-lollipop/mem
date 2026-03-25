#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace VirusExecutorService {

constexpr int32_t VES_CONTROL_SA_ID = 1251;
constexpr uint32_t VES_OPEN_SESSION_REQUEST_VERSION = 1;
constexpr size_t VES_OPEN_SESSION_MAX_ENGINE_KINDS = 32;

enum class VesEngineKind : uint32_t {
    Scan = 1,
};

enum class VesHeartbeatStatus : uint8_t {
    OkIdle = 0,
    OkBusy = 1,
    DegradedLongRunning = 2,
    UnhealthyNoSession = 3,
    UnhealthyStopping = 4,
    UnhealthyInternalError = 5,
};

enum class VesHeartbeatReasonCode : uint8_t {
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
    uint16_t priority = static_cast<uint16_t>(MemRpc::Priority::Normal);
    uint32_t timeoutMs = 0;
    std::vector<uint8_t> payload;
};

struct VesAnyCallReply {
    MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
    int32_t errorCode = 0;
    std::vector<uint8_t> payload;
};

struct VesOpenSessionRequest {
    uint32_t version = VES_OPEN_SESSION_REQUEST_VERSION;
    std::vector<uint32_t> engineKinds;
};

inline VesOpenSessionRequest DefaultVesOpenSessionRequest()
{
    return {};
}

inline std::vector<uint32_t> NormalizeVesEngineKinds(std::vector<uint32_t> engineKinds)
{
    std::sort(engineKinds.begin(), engineKinds.end());
    engineKinds.erase(std::unique(engineKinds.begin(), engineKinds.end()), engineKinds.end());
    return engineKinds;
}

inline bool IsValidVesOpenSessionRequest(const VesOpenSessionRequest& request)
{
    if (request.version != VES_OPEN_SESSION_REQUEST_VERSION ||
        request.engineKinds.size() > VES_OPEN_SESSION_MAX_ENGINE_KINDS) {
        return false;
    }
    return std::all_of(request.engineKinds.begin(), request.engineKinds.end(), [](uint32_t engineKind) {
        return engineKind != 0;
    });
}

class IVirusProtectionExecutor : public OHOS::IRemoteBroker {
public:
    ~IVirusProtectionExecutor() override = default;
    virtual MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles) = 0;
    virtual MemRpc::StatusCode CloseSession() = 0;
    virtual MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) = 0;
    virtual MemRpc::StatusCode AnyCall(const VesAnyCallRequest& request, VesAnyCallReply& reply) = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_INTERFACE_H_
