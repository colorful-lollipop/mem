#include <gtest/gtest.h>

#include "core/protocol.h"
#include "client/replay_classifier.h"

namespace memrpc {

TEST(ReplayClassifierTest, MapsRuntimeStates) {
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Admitted), ReplayHint::SafeToReplay);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Queued), ReplayHint::SafeToReplay);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Executing), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Responding), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Ready), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Consumed), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Free), ReplayHint::MaybeExecuted);
}

TEST(ReplayClassifierTest, ConvertsToRpcRuntimeState) {
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Free), RpcRuntimeState::Free);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Admitted), RpcRuntimeState::Admitted);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Queued), RpcRuntimeState::Queued);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Executing), RpcRuntimeState::Executing);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Responding), RpcRuntimeState::Responding);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Ready), RpcRuntimeState::Ready);
  EXPECT_EQ(ToRpcRuntimeState(SlotRuntimeStateCode::Consumed), RpcRuntimeState::Consumed);
}

}  // namespace memrpc
