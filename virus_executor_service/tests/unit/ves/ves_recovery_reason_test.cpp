#include <gtest/gtest.h>

#include "transport/ves_control_interface.h"

namespace VirusExecutorService {

TEST(VesRecoveryReasonTest, HeartbeatReplyDefaultsRemainConservative) {
    VesHeartbeatReply reply{};
    EXPECT_EQ(reply.version, 1u);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::Unhealthy));
    EXPECT_EQ(reply.sessionId, 0u);
}

}  // namespace VirusExecutorService
