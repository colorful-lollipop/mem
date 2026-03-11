#include "virus_executor_service.h"

#include <thread>

#include "iservice_registry.h"
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
    auto status = session_service_->CloseSession();

    // After releasing session resources, trigger self-unload asynchronously.
    // Must be async because the IPC caller is still waiting for our reply.
    std::thread([sa_id = GetSystemAbilityId()]() {
        HLOGI("requesting self-unload for sa_id=%{public}d", sa_id);
        auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
        if (sam != nullptr) {
            sam->UnloadSystemAbility(sa_id);
        }
    }).detach();

    return status;
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
