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
        &handles->shm_fd,
        &handles->high_req_event_fd,
        &handles->normal_req_event_fd,
        &handles->resp_event_fd,
        &handles->req_credit_event_fd,
        &handles->resp_credit_event_fd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}
}  // namespace

EngineSessionService::EngineSessionService(VpsDemoService* service)
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
        HLOGE("bootstrap OpenSession failed");
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
        HLOGE("RpcServer start failed");
        return start_status;
    }

    if (service_ != nullptr) {
        service_->Initialize();
    }
    initialized_ = true;
    HLOGI("EngineSessionService initialized");
    return memrpc::StatusCode::Ok;
}

memrpc::StatusCode EngineSessionService::OpenSession(memrpc::BootstrapHandles& handles) {
    const memrpc::StatusCode init_status = EnsureInitialized();
    if (init_status != memrpc::StatusCode::Ok) {
        return init_status;
    }
    return bootstrap_->OpenSession(handles);
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
    HLOGI("EngineSessionService closed");
    return memrpc::StatusCode::Ok;
}

}  // namespace vpsdemo
