#include "service/virus_executor_service.h"

#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

#include "iservice_registry.h"
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
    bool shouldRequestUnload = false;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = session_service_;
        shouldRequestUnload = sessionService != nullptr &&
                              !stopping_.load() &&
                              !unloadRequested_ &&
                              published_;
        if (shouldRequestUnload) {
            unloadRequested_ = true;
        }
    }
    if (!sessionService) {
        return MemRpc::StatusCode::Ok;
    }
    auto status = sessionService->CloseSession();

    // After releasing session resources, trigger self-unload asynchronously.
    // Must be async because the IPC caller is still waiting for our reply.
    if (shouldRequestUnload) {
        std::thread([sa_id = GetSystemAbilityId()]() {
            HILOGI("requesting self-unload for sa_id=%{public}d", sa_id);
            auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
            if (sam != nullptr) {
                sam->UnloadSystemAbility(sa_id);
            }
        }).detach();
    }

    return status;
}

MemRpc::StatusCode VirusExecutorService::Heartbeat(VesHeartbeatReply& reply) {
    reply = VesHeartbeatReply{};
    if (stopping_.load()) {
        reply.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyStopping);
        reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::Stopping);
        return MemRpc::StatusCode::Ok;
    }
    std::shared_ptr<EngineSessionService> sessionService;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        sessionService = session_service_;
    }
    if (!sessionService) {
        if (service_.initialized()) {
            reply.flags |= VES_HEARTBEAT_FLAG_INITIALIZED;
        }
        std::snprintf(reply.currentTask, sizeof(reply.currentTask), "%s", "idle");
        return MemRpc::StatusCode::Ok;
    }
    const auto snapshot = service_.GetHealthSnapshot();
    const uint64_t sessionId = sessionService->session_id();
    if (!service_.initialized()) {
        reply.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyInternalError);
        reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::InternalError);
        return MemRpc::StatusCode::Ok;
    }
    if (sessionId == 0) {
        reply.flags |= VES_HEARTBEAT_FLAG_INITIALIZED;
        reply.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession);
        reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession);
        return MemRpc::StatusCode::Ok;
    }
    PopulateHealthyReply(snapshot, sessionId, &reply);
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
    }
    if (sessionService) {
        sessionService->CloseSession();
    }
}

}  // namespace VirusExecutorService
