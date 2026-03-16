#include "service/virus_executor_service.h"

#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

#include "iservice_registry.h"
#include "memrpc/core/codec.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {

bool IsTestkitFaultInjectionEnabled() {
    const char* value = std::getenv("VES_ENABLE_TESTKIT_FAULTS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

void PopulateHealthyReply(const VesHealthSnapshot& snapshot, uint64_t sessionId,
                          VesHeartbeatReply* reply)
{
    if (reply == nullptr) {
        return;
    }
    reply->sessionId = sessionId;
    reply->status = snapshot.status;
    reply->reasonCode = snapshot.reasonCode;
    reply->inFlight = snapshot.inFlight;
    reply->lastTaskAgeMs = snapshot.lastTaskAgeMs;
    std::snprintf(reply->currentTask, sizeof(reply->currentTask), "%s",
                  snapshot.currentTask.c_str());
    reply->flags = snapshot.flags | VES_HEARTBEAT_FLAG_HAS_SESSION;
}

}  // namespace

VirusExecutorService::VirusExecutorService()
    : OHOS::SystemAbility(VES_CONTROL_SA_ID, true) {}

bool VirusExecutorService::Publish(VirusExecutorService* service) {
    const bool published = OHOS::SystemAbility::Publish(service);
    if (!published) {
        return false;
    }
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    published_ = true;
    return true;
}

MemRpc::StatusCode VirusExecutorService::OpenSession(MemRpc::BootstrapHandles& handles) {
    std::shared_ptr<EngineSessionService> sessionService;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = session_service_;
    }
    if (!sessionService) {
        HILOGE("session service not initialized");
        return MemRpc::StatusCode::InvalidArgument;
    }
    return sessionService->OpenSession(handles);
}

MemRpc::StatusCode VirusExecutorService::CloseSession() {
    std::shared_ptr<EngineSessionService> sessionService;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = session_service_;
    }
    if (!sessionService) {
        return MemRpc::StatusCode::Ok;
    }
    return sessionService->CloseSession();
}

MemRpc::StatusCode VirusExecutorService::Heartbeat(VesHeartbeatReply& heartbeat) {
    heartbeat = VesHeartbeatReply{};
    if (stopping_.load()) {
        heartbeat.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyStopping);
        heartbeat.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::Stopping);
        return MemRpc::StatusCode::Ok;
    }
    std::shared_ptr<EngineSessionService> sessionService;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = session_service_;
    }
    if (!sessionService) {
        if (service_.initialized()) {
            heartbeat.flags |= VES_HEARTBEAT_FLAG_INITIALIZED;
        }
        std::snprintf(heartbeat.currentTask, sizeof(heartbeat.currentTask), "%s", "idle");
        return MemRpc::StatusCode::Ok;
    }
    const auto snapshot = service_.GetHealthSnapshot();
    const uint64_t sessionId = sessionService->session_id();
    if (!service_.initialized()) {
        heartbeat.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyInternalError);
        heartbeat.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::InternalError);
        return MemRpc::StatusCode::Ok;
    }
    if (sessionId == 0) {
        heartbeat.flags |= VES_HEARTBEAT_FLAG_INITIALIZED;
        heartbeat.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession);
        heartbeat.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession);
        return MemRpc::StatusCode::Ok;
    }
    PopulateHealthyReply(snapshot, sessionId, &heartbeat);
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VirusExecutorService::AnyCall(const VesAnyCallRequest& request,
                                                 VesAnyCallReply& vesReply) {
    vesReply = VesAnyCallReply{};
    if (!service_.initialized()) {
        vesReply.status = MemRpc::StatusCode::PeerDisconnected;
        return MemRpc::StatusCode::Ok;
    }

    switch (static_cast<VesOpcode>(request.opcode)) {
        case VesOpcode::ScanFile: {
            ScanTask scanRequest;
            if (!MemRpc::DecodeMessage<ScanTask>(request.payload, &scanRequest)) {
                vesReply.status = MemRpc::StatusCode::ProtocolMismatch;
                return MemRpc::StatusCode::Ok;
            }
            ScanFileReply scanReply = service_.ScanFile(scanRequest);
            vesReply.status = MemRpc::StatusCode::Ok;
            if (!MemRpc::EncodeMessage<ScanFileReply>(scanReply, &vesReply.payload)) {
                vesReply.status = MemRpc::StatusCode::EngineInternalError;
                vesReply.payload.clear();
            }
            return MemRpc::StatusCode::Ok;
        }
    }

    vesReply.status = MemRpc::StatusCode::InvalidArgument;
    return MemRpc::StatusCode::Ok;
}

void VirusExecutorService::OnStart() {
    HILOGI("OnStart sa_id=%{public}d", GetSystemAbilityId());
    stopping_.store(false);
    testkitService_.SetFaultInjectionEnabled(IsTestkitFaultInjectionEnabled());
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        published_ = false;
        unloadRequested_ = false;
        session_service_ =
            std::make_shared<EngineSessionService>(
                std::vector<RpcHandlerRegistrar*>{&service_, &testkitService_});
        service_.SetEventPublisher(session_service_);
    }
    service_.Initialize();
}

void VirusExecutorService::OnStop() {
    HILOGI("OnStop");
    stopping_.store(true);
    std::shared_ptr<EngineSessionService> sessionService;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = std::move(session_service_);
        service_.SetEventPublisher({});
    }
    if (sessionService) {
        sessionService->CloseSession();
    }
    OHOS::SystemAbility::OnStop();
}

}  // namespace VirusExecutorService
