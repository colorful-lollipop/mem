#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/types.h"
#include "ves_session_service.h"
#include "vpsdemo_service.h"

namespace vpsdemo {

namespace {
void CloseHandles(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return;
    }
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

TEST(VpsSessionServiceTest, OpenSessionInitializesService) {
    VpsDemoService service;
    EngineSessionService sessionService(&service);

    memrpc::BootstrapHandles handles{};
    EXPECT_EQ(sessionService.OpenSession(&handles), memrpc::StatusCode::Ok);
    EXPECT_TRUE(service.initialized());

    CloseHandles(&handles);
}

}  // namespace vpsdemo
