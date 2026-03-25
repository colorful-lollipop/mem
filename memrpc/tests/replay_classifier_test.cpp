#include <gtest/gtest.h>

#include "client/replay_classifier.h"
#include "memrpc/core/protocol.h"

namespace MemRpc {

TEST(ReplayClassifierTest, MapsRuntimeStates)
{
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Admitted), ReplayHint::SafeToReplay);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Queued), ReplayHint::SafeToReplay);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Executing), ReplayHint::MaybeExecuted);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Responding), ReplayHint::MaybeExecuted);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Ready), ReplayHint::MaybeExecuted);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Consumed), ReplayHint::MaybeExecuted);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Free), ReplayHint::MaybeExecuted);
    EXPECT_EQ(ClassifyReplayHint(RpcRuntimeState::Unknown), ReplayHint::Unknown);
}

}  // namespace MemRpc
