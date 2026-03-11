#include "virus_executor_service.h"

#include "virus_protection_service_log.h"

namespace vpsdemo {

VirusExecutorService::VirusExecutorService()
    : OHOS::SystemAbility(VPS_BOOTSTRAP_SA_ID, true) {}

memrpc::StatusCode VirusExecutorService::OpenSession(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return memrpc::StatusCode::InvalidArgument;
    }
    if (!session_service_) {
        HLOGE("session service not initialized");
        return memrpc::StatusCode::InvalidArgument;
    }
    return session_service_->OpenSession(handles);
}

memrpc::StatusCode VirusExecutorService::CloseSession() {
    if (!session_service_) {
        return memrpc::StatusCode::Ok;
    }
    return session_service_->CloseSession();
}

void VirusExecutorService::OnStart() {
    HLOGI("OnStart sa_id=%{public}d", GetSystemAbilityId());
    session_service_ = std::make_shared<EngineSessionService>(&service_);
}

void VirusExecutorService::OnStop() {
    HLOGI("OnStop");
    if (session_service_) {
        session_service_->CloseSession();
        session_service_.reset();
    }
}

}  // namespace vpsdemo
