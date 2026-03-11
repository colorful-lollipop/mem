#include "virus_executor_service.h"

#include <cstring>
#include <iostream>

#include "virus_protection_service_log.h"

namespace vpsdemo {

namespace {

struct SessionMetadata {
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

}  // namespace

VirusExecutorService::VirusExecutorService(std::shared_ptr<VpsSessionProvider> provider)
    : OHOS::SystemAbility(VPS_BOOTSTRAP_SA_ID, true),
      provider_(std::move(provider)) {}

VirusExecutorService::~VirusExecutorService() {
    transport_.Stop();
}

memrpc::StatusCode VirusExecutorService::OpenSession(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return memrpc::StatusCode::InvalidArgument;
    }
    return provider_->OpenSession(handles);
}

memrpc::StatusCode VirusExecutorService::CloseSession() {
    return provider_->CloseSession();
}

void VirusExecutorService::OnStart() {
    HLOGI("OnStart sa_id=%{public}d", GetSystemAbilityId());

    const std::string& path = AsObject()->GetServicePath();
    if (path.empty()) {
        HLOGE("no service path set, cannot start transport");
        return;
    }

    auto handler = [this](int command, OHOS::MockIpcReply* reply) -> bool {
        if (command != 1) {
            return false;
        }

        memrpc::BootstrapHandles handles{};
        memrpc::StatusCode status = provider_->OpenSession(&handles);
        if (status != memrpc::StatusCode::Ok) {
            HLOGE("provider OpenSession failed");
            return false;
        }

        constexpr size_t FD_COUNT = 6;
        reply->fds[0] = handles.shm_fd;
        reply->fds[1] = handles.high_req_event_fd;
        reply->fds[2] = handles.normal_req_event_fd;
        reply->fds[3] = handles.resp_event_fd;
        reply->fds[4] = handles.req_credit_event_fd;
        reply->fds[5] = handles.resp_credit_event_fd;
        reply->fd_count = FD_COUNT;

        SessionMetadata meta{};
        meta.protocol_version = handles.protocol_version;
        meta.session_id = handles.session_id;
        std::memcpy(reply->data, &meta, sizeof(meta));
        reply->data_len = sizeof(meta);

        return true;
    };

    if (!transport_.Start(path, std::move(handler))) {
        HLOGE("failed to start MockServiceSocket at %{public}s", path.c_str());
    }
}

void VirusExecutorService::OnStop() {
    HLOGI("OnStop");
    transport_.Stop();
}

}  // namespace vpsdemo
