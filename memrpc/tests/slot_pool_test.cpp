#include <gtest/gtest.h>

#include <type_traits>
#include <vector>

#include "core/slot_pool.h"

static_assert(!std::is_constructible_v<MemRpc::SlotPool, uint32_t, uint32_t>,
              "SlotPool must not accept reserved-slot constructor");

TEST(SlotPoolTest, ReserveTransitionAndReleaseLifecycle) {
  MemRpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 0u);
  EXPECT_EQ(pool.GetState(*first), MemRpc::SlotState::Reserved);

  EXPECT_TRUE(pool.Transition(*first, MemRpc::SlotState::QueuedNormal));
  EXPECT_EQ(pool.GetState(*first), MemRpc::SlotState::QueuedNormal);
  EXPECT_TRUE(pool.Transition(*first, MemRpc::SlotState::Dispatched));
  EXPECT_TRUE(pool.Transition(*first, MemRpc::SlotState::Processing));
  EXPECT_TRUE(pool.Transition(*first, MemRpc::SlotState::Responded));
  EXPECT_TRUE(pool.Release(*first));
  EXPECT_EQ(pool.GetState(*first), MemRpc::SlotState::Free);
}

TEST(SlotPoolTest, ReserveFailsWhenExhausted) {
  MemRpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(pool.Reserve().has_value());
}

TEST(SlotPoolTest, InvalidTransitionsAndInvalidReleaseAreRejected) {
  MemRpc::SlotPool pool(2);

  const auto slot = pool.Reserve();
  ASSERT_TRUE(slot.has_value());
  EXPECT_FALSE(pool.Transition(*slot, MemRpc::SlotState::Processing));
  EXPECT_FALSE(pool.Release(99u));
  EXPECT_TRUE(pool.Release(*slot));
}

TEST(SlotPoolTest, SharedSlotPoolSharesFreeListAcrossViews) {
  std::vector<uint8_t> region(MemRpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(MemRpc::InitializeSharedSlotPool(region.data(), 2));

  MemRpc::SharedSlotPool producer(region.data());
  MemRpc::SharedSlotPool consumer(region.data());

  const auto first = producer.Reserve();
  const auto second = producer.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(producer.Reserve().has_value());

  EXPECT_TRUE(consumer.Release(*first));
  const auto recycled = producer.Reserve();
  ASSERT_TRUE(recycled.has_value());
  EXPECT_EQ(*recycled, *first);
}

TEST(SlotPoolTest, SharedSlotPoolRejectsReleaseOfNeverReservedSlot) {
  std::vector<uint8_t> region(MemRpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(MemRpc::InitializeSharedSlotPool(region.data(), 2));

  MemRpc::SharedSlotPool pool(region.data());
  const auto reserved = pool.Reserve();
  ASSERT_TRUE(reserved.has_value());

  const uint32_t other_slot = *reserved == 0 ? 1u : 0u;
  EXPECT_FALSE(pool.Release(other_slot));
  EXPECT_EQ(pool.Available(), 1u);
}

TEST(SlotPoolTest, SharedSlotPoolRejectsDuplicateReleaseBeforeCapacityIsFull) {
  std::vector<uint8_t> region(MemRpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(MemRpc::InitializeSharedSlotPool(region.data(), 2));

  MemRpc::SharedSlotPool pool(region.data());
  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  ASSERT_TRUE(pool.Release(*first));
  EXPECT_FALSE(pool.Release(*first));
  EXPECT_EQ(pool.Available(), 1u);
}
