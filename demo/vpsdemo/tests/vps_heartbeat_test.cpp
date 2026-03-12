#include <gtest/gtest.h>

#include "virus_executor_service.h"
#include "vps_bootstrap_interface.h"

namespace vpsdemo {

TEST(VpsHeartbeatTest, UnhealthyBeforeOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    VpsHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VpsHeartbeatStatus::Unhealthy));

    service.OnStop();
}

TEST(VpsHeartbeatTest, OkAfterOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), memrpc::StatusCode::Ok);
    const uint64_t session_id = handles.session_id;

    VpsHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VpsHeartbeatStatus::Ok));
    EXPECT_EQ(reply.session_id, session_id);
    EXPECT_STREQ(reply.current_task, "idle");

    service.CloseSession();
    service.OnStop();
}

}  // namespace vpsdemo
