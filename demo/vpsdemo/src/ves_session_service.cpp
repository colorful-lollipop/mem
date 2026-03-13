#include "vpsdemo/ves/ves_session_service.h"

#include <utility>
#include <unistd.h>

#include "memrpc/server/rpc_server.h"
#include "vpsdemo/ves/ves_engine_service.h"
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

EngineSessionService::EngineSessionService(std::vector<RpcHandlerRegistrar*> registrars)
    : registrars_(std::move(registrars)) {}

memrpc::StatusCode EngineSessionService::EnsureInitialized() {
    std::lock_guard<std::mutex> lock(initMutex_);
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
    rpcServer_ = std::make_unique<memrpc::RpcServer>(serverHandles);
    for (auto* registrar : registrars_) {
        if (registrar != nullptr) {
            registrar->RegisterHandlers(rpcServer_.get());
        }
    }
    const memrpc::StatusCode start_status = rpcServer_->Start();
    if (start_status != memrpc::StatusCode::Ok) {
        HILOGE("RpcServer start failed");
        return start_status;
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
        sessionId_ = handles.sessionId;
    }
    return status;
}

memrpc::StatusCode EngineSessionService::CloseSession() {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (!initialized_) {
        return memrpc::StatusCode::Ok;
    }
    if (rpcServer_) {
        rpcServer_->Stop();
        rpcServer_.reset();
    }
    bootstrap_.reset();
    initialized_ = false;
    sessionId_ = 0;
    HILOGI("EngineSessionService closed");
    return memrpc::StatusCode::Ok;
}

uint64_t EngineSessionService::session_id() const {
    return sessionId_;
}

}  // namespace vpsdemo
