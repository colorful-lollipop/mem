#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/types.h"
#include "ves_session_service.h"
#include "ves_engine_service.h"

namespace vpsdemo {

namespace {
void CloseHandles(memrpc::BootstrapHandles* handles) {
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

TEST(VesSessionServiceTest, OpenSessionInitializesService) {
    VesEngineService service;
    EngineSessionService sessionService(&service);

    memrpc::BootstrapHandles handles{};
    EXPECT_EQ(sessionService.OpenSession(handles), memrpc::StatusCode::Ok);
    EXPECT_TRUE(service.initialized());

    CloseHandles(&handles);
}

}  // namespace vpsdemo
