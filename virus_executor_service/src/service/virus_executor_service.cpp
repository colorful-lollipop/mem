#include "service/virus_executor_service.h"

#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

#include "iservice_registry.h"
#include "virus_protection_service_log.h"

namespace virus_executor_service {

namespace {

bool IsTestkitFaultInjectionEnabled() {
    const char* value = std::getenv("VES_ENABLE_TESTKIT_FAULTS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

}  // namespace

VirusExecutorService::VirusExecutorService()
    : OHOS::SystemAbility(VES_BOOTSTRAP_SA_ID, true) {}

MemRpc::StatusCode VirusExecutorService::OpenSession(MemRpc::BootstrapHandles& handles) {
    if (!session_service_) {
        HILOGE("session service not initialized");
        return MemRpc::StatusCode::InvalidArgument;
    }
    return session_service_->OpenSession(handles);
}

MemRpc::StatusCode VirusExecutorService::CloseSession() {
    if (!session_service_) {
        return MemRpc::StatusCode::Ok;
    }
    auto status = session_service_->CloseSession();

    // After releasing session resources, trigger self-unload asynchronously.
    // Must be async because the IPC caller is still waiting for our reply.
    std::thread([sa_id = GetSystemAbilityId()]() {
        HILOGI("requesting self-unload for sa_id=%{public}d", sa_id);
        auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
        if (sam != nullptr) {
            sam->UnloadSystemAbility(sa_id);
        }
    }).detach();

    return status;
}

MemRpc::StatusCode VirusExecutorService::Heartbeat(VesHeartbeatReply& reply) {
    reply = VesHeartbeatReply{};
    if (!session_service_) {
        return MemRpc::StatusCode::Ok;
    }
    reply.sessionId = session_service_->session_id();
    const auto snapshot = service_.GetHealthSnapshot();
    reply.inFlight = snapshot.inFlight;
    reply.lastTaskAgeMs = snapshot.lastTaskAgeMs;
    std::snprintf(reply.currentTask, sizeof(reply.currentTask), "%s",
                  snapshot.currentTask.c_str());

    const bool healthy = service_.initialized() && reply.sessionId != 0;
    reply.status = static_cast<uint32_t>(healthy ? VesHeartbeatStatus::Ok
                                                 : VesHeartbeatStatus::Unhealthy);
    return MemRpc::StatusCode::Ok;
}

void VirusExecutorService::OnStart() {
    HILOGI("OnStart sa_id=%{public}d", GetSystemAbilityId());
    testkitService_.SetFaultInjectionEnabled(IsTestkitFaultInjectionEnabled());
    session_service_ =
        std::make_shared<EngineSessionService>(
            std::vector<RpcHandlerRegistrar*>{&service_, &testkitService_});
    service_.Initialize();
}

void VirusExecutorService::OnStop() {
    HILOGI("OnStop");
    if (session_service_) {
        session_service_->CloseSession();
        session_service_.reset();
    }
}

}  // namespace virus_executor_service
