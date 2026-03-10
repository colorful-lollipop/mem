#include <gtest/gtest.h>

#include <type_traits>
#include <vector>

#include "core/slot_pool.h"

static_assert(!std::is_constructible_v<memrpc::SlotPool, uint32_t, uint32_t>,
              "SlotPool must not accept reserved-slot constructor");

TEST(SlotPoolTest, ReserveTransitionAndReleaseLifecycle) {
  memrpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 0u);
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::Reserved);

  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::QueuedNormal));
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::QueuedNormal);
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::Dispatched));
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::Processing));
  EXPECT_TRUE(pool.Transition(*first, memrpc::SlotState::Responded));
  EXPECT_TRUE(pool.Release(*first));
  EXPECT_EQ(pool.GetState(*first), memrpc::SlotState::Free);
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
  EXPECT_FALSE(pool.Transition(*slot, memrpc::SlotState::Processing));
  EXPECT_FALSE(pool.Release(99u));
  EXPECT_TRUE(pool.Release(*slot));
}

TEST(SlotPoolTest, SharedSlotPoolSharesFreeListAcrossViews) {
  std::vector<uint8_t> region(memrpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(memrpc::InitializeSharedSlotPool(region.data(), 2));

  memrpc::SharedSlotPool producer(region.data());
  memrpc::SharedSlotPool consumer(region.data());

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
  std::vector<uint8_t> region(memrpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(memrpc::InitializeSharedSlotPool(region.data(), 2));

  memrpc::SharedSlotPool pool(region.data());
  const auto reserved = pool.Reserve();
  ASSERT_TRUE(reserved.has_value());

  const uint32_t other_slot = *reserved == 0 ? 1u : 0u;
  EXPECT_FALSE(pool.Release(other_slot));
  EXPECT_EQ(pool.available(), 1u);
}

TEST(SlotPoolTest, SharedSlotPoolRejectsDuplicateReleaseBeforeCapacityIsFull) {
  std::vector<uint8_t> region(memrpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(memrpc::InitializeSharedSlotPool(region.data(), 2));

  memrpc::SharedSlotPool pool(region.data());
  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  ASSERT_TRUE(pool.Release(*first));
  EXPECT_FALSE(pool.Release(*first));
  EXPECT_EQ(pool.available(), 1u);
}

TEST(SlotPoolTest, NormalReservePreservesHighReservedSlots) {
  memrpc::SlotPool pool(3, 1);

  const auto first_normal = pool.Reserve(memrpc::Priority::Normal);
  const auto second_normal = pool.Reserve(memrpc::Priority::Normal);
  ASSERT_TRUE(first_normal.has_value());
  ASSERT_TRUE(second_normal.has_value());
  EXPECT_FALSE(pool.Reserve(memrpc::Priority::Normal).has_value());

  const auto high = pool.Reserve(memrpc::Priority::High);
  ASSERT_TRUE(high.has_value());
}
