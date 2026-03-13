#include "ves_session_service.h"

#include <unistd.h>

#include "memrpc/server/rpc_server.h"
#include "vpsdemo_service.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

namespace {
void CloseHandles(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) return;
    int* fds[] = {
        &handles->shmFd,
        &handles->highReqEventFd,
        &handles->normalReqEventFd,
        &handles->respEventFd,
        &handles->reqCreditEventFd,
        &handles->respCreditEventFd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}
}  // namespace

EngineSessionService::EngineSessionService(VesEngineService* service)
    : service_(service) {}

memrpc::StatusCode EngineSessionService::EnsureInitialized() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (initialized_) {
        return memrpc::StatusCode::Ok;
    }

    if (!bootstrap_) {
        bootstrap_ = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    }

    memrpc::BootstrapHandles throwaway{};
    const memrpc::StatusCode open_status = bootstrap_->OpenSession(throwaway);
    if (open_status != memrpc::StatusCode::Ok) {
        HILOGE("bootstrap OpenSession failed");
        CloseHandles(&throwaway);
        return open_status;
    }
    CloseHandles(&throwaway);

    const memrpc::BootstrapHandles serverHandles = bootstrap_->serverHandles();
    rpc_server_ = std::make_unique<memrpc::RpcServer>(serverHandles);
    if (service_ != nullptr) {
        service_->RegisterHandlers(rpc_server_.get());
    }
    const memrpc::StatusCode start_status = rpc_server_->Start();
    if (start_status != memrpc::StatusCode::Ok) {
        HILOGE("RpcServer start failed");
        return start_status;
    }

    if (service_ != nullptr) {
        service_->Initialize();
    }
    initialized_ = true;
    HILOGI("EngineSessionService initialized");
    return memrpc::StatusCode::Ok;
}

memrpc::StatusCode EngineSessionService::OpenSession(memrpc::BootstrapHandles& handles) {
    const memrpc::StatusCode init_status = EnsureInitialized();
    if (init_status != memrpc::StatusCode::Ok) {
        return init_status;
    }
    auto status = bootstrap_->OpenSession(handles);
    if (status == memrpc::StatusCode::Ok) {
        session_id_ = handles.session_id;
    }
    return status;
}

memrpc::StatusCode EngineSessionService::CloseSession() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (!initialized_) {
        return memrpc::StatusCode::Ok;
    }
    if (rpc_server_) {
        rpc_server_->Stop();
        rpc_server_.reset();
    }
    bootstrap_.reset();
    initialized_ = false;
    session_id_ = 0;
    HILOGI("EngineSessionService closed");
    return memrpc::StatusCode::Ok;
}

uint64_t EngineSessionService::session_id() const {
    return session_id_;
}

}  // namespace vpsdemo
