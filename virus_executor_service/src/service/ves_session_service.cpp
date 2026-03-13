#include "ves/ves_session_service.h"

#include <utility>
#include <unistd.h>

#include "memrpc/server/rpc_server.h"
#include "ves/ves_engine_service.h"
#include "virus_protection_service_log.h"

namespace virus_executor_service {

namespace {
void CloseHandles(MemRpc::BootstrapHandles* handles) {
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

MemRpc::StatusCode EngineSessionService::EnsureInitialized() {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (initialized_) {
        return MemRpc::StatusCode::Ok;
    }

    if (!bootstrap_) {
        bootstrap_ = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    }

    MemRpc::BootstrapHandles throwaway{};
    const MemRpc::StatusCode open_status = bootstrap_->OpenSession(throwaway);
    if (open_status != MemRpc::StatusCode::Ok) {
        HILOGE("bootstrap OpenSession failed");
        CloseHandles(&throwaway);
        return open_status;
    }
    CloseHandles(&throwaway);

    const MemRpc::BootstrapHandles serverHandles = bootstrap_->serverHandles();
    rpcServer_ = std::make_unique<MemRpc::RpcServer>(serverHandles);
    for (auto* registrar : registrars_) {
        if (registrar != nullptr) {
            registrar->RegisterHandlers(rpcServer_.get());
        }
    }
    const MemRpc::StatusCode start_status = rpcServer_->Start();
    if (start_status != MemRpc::StatusCode::Ok) {
        HILOGE("RpcServer start failed");
        return start_status;
    }

    initialized_ = true;
    HILOGI("EngineSessionService initialized");
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode EngineSessionService::OpenSession(MemRpc::BootstrapHandles& handles) {
    const MemRpc::StatusCode init_status = EnsureInitialized();
    if (init_status != MemRpc::StatusCode::Ok) {
        return init_status;
    }
    auto status = bootstrap_->OpenSession(handles);
    if (status == MemRpc::StatusCode::Ok) {
        sessionId_ = handles.sessionId;
    }
    return status;
}

MemRpc::StatusCode EngineSessionService::CloseSession() {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (!initialized_) {
        return MemRpc::StatusCode::Ok;
    }
    if (rpcServer_) {
        rpcServer_->Stop();
        rpcServer_.reset();
    }
    bootstrap_.reset();
    initialized_ = false;
    sessionId_ = 0;
    HILOGI("EngineSessionService closed");
    return MemRpc::StatusCode::Ok;
}

uint64_t EngineSessionService::session_id() const {
    return sessionId_;
}

}  // namespace virus_executor_service
