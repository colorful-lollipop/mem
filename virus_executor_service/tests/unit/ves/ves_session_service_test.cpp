#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/types.h"
#include "ves/ves_session_service.h"
#include "ves/ves_engine_service.h"

namespace virus_executor_service {

namespace {
void CloseHandles(MemRpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return;
    }
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

TEST(VesSessionServiceTest, OpenSessionCreatesSessionHandles) {
    VesEngineService service;
    EngineSessionService sessionService({&service});

    MemRpc::BootstrapHandles handles{};
    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);

    CloseHandles(&handles);
}

TEST(VesSessionServiceTest, OpenSessionLeavesRegistrarStateUntouched) {
    VesEngineService service;
    EXPECT_FALSE(service.initialized());

    EngineSessionService sessionService({&service});
    MemRpc::BootstrapHandles handles{};

    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_FALSE(service.initialized());

    CloseHandles(&handles);
}

}  // namespace virus_executor_service
