#include <gtest/gtest.h>

#include "core/slot_pool.h"

TEST(SlotPoolTest, ReserveTransitionAndReleaseLifecycle) {
  memrpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 0u);
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::kReserved);

  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::kQueuedNormal));
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::kQueuedNormal);
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::kDispatched));
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::kProcessing));
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::kResponded));
  EXPECT_TRUE(pool.Release(*first));
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::kFree);
}

TEST(SlotPoolTest, ReserveFailsWhenExhausted) {
  memrpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(pool.Reserve().has_value());
}

TEST(SlotPoolTest, InvalidTransitionsAndInvalidReleaseAreRejected) {
  memrpc::SlotPool pool(2);

  const auto slot = pool.Reserve();
  ASSERT_TRUE(slot.has_value());
  EXPECT_FALSE(pool.Transition(*slot, memrpc::SlotState::kProcessing));
  EXPECT_FALSE(pool.Release(99u));
  EXPECT_TRUE(pool.Release(*slot));
}
