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

}  // namespace vpsdemo
