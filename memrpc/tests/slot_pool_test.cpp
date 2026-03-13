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
  const uint32_t first_slot = first.value_or(UINT32_MAX);
  EXPECT_EQ(first_slot, 0U);
  EXPECT_EQ(pool.GetState(first_slot), MemRpc::SlotState::Reserved);

  EXPECT_TRUE(pool.Transition(first_slot, MemRpc::SlotState::QueuedNormal));
  EXPECT_EQ(pool.GetState(first_slot), MemRpc::SlotState::QueuedNormal);
  EXPECT_TRUE(pool.Transition(first_slot, MemRpc::SlotState::Dispatched));
  EXPECT_TRUE(pool.Transition(first_slot, MemRpc::SlotState::Processing));
  EXPECT_TRUE(pool.Transition(first_slot, MemRpc::SlotState::Responded));
  EXPECT_TRUE(pool.Release(first_slot));
  EXPECT_EQ(pool.GetState(first_slot), MemRpc::SlotState::Free);
}

TEST(SlotPoolTest, ReserveFailsWhenExhausted) {
  MemRpc::SlotPool pool(2);

  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first.value_or(UINT32_MAX), second.value_or(UINT32_MAX));
  EXPECT_FALSE(pool.Reserve().has_value());
}

TEST(SlotPoolTest, InvalidTransitionsAndInvalidReleaseAreRejected) {
  MemRpc::SlotPool pool(2);

  const auto slot = pool.Reserve();
  ASSERT_TRUE(slot.has_value());
  const uint32_t reserved_slot = slot.value_or(UINT32_MAX);
  EXPECT_FALSE(pool.Transition(reserved_slot, MemRpc::SlotState::Processing));
  EXPECT_FALSE(pool.Release(99U));
  EXPECT_TRUE(pool.Release(reserved_slot));
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
  const uint32_t first_slot = first.value_or(UINT32_MAX);
  EXPECT_FALSE(producer.Reserve().has_value());

  EXPECT_TRUE(consumer.Release(first_slot));
  const auto recycled = producer.Reserve();
  ASSERT_TRUE(recycled.has_value());
  EXPECT_EQ(recycled.value_or(UINT32_MAX), first_slot);
}

TEST(SlotPoolTest, SharedSlotPoolRejectsReleaseOfNeverReservedSlot) {
  std::vector<uint8_t> region(MemRpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(MemRpc::InitializeSharedSlotPool(region.data(), 2));

  MemRpc::SharedSlotPool pool(region.data());
  const auto reserved = pool.Reserve();
  ASSERT_TRUE(reserved.has_value());

  const uint32_t reserved_slot = reserved.value_or(UINT32_MAX);
  const uint32_t other_slot = reserved_slot == 0U ? 1U : 0U;
  EXPECT_FALSE(pool.Release(other_slot));
  EXPECT_EQ(pool.Available(), 1U);
}

TEST(SlotPoolTest, SharedSlotPoolRejectsDuplicateReleaseBeforeCapacityIsFull) {
  std::vector<uint8_t> region(MemRpc::ComputeSharedSlotPoolBytes(2), 0);
  ASSERT_TRUE(MemRpc::InitializeSharedSlotPool(region.data(), 2));

  MemRpc::SharedSlotPool pool(region.data());
  const auto first = pool.Reserve();
  const auto second = pool.Reserve();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const uint32_t first_slot = first.value_or(UINT32_MAX);

  ASSERT_TRUE(pool.Release(first_slot));
  EXPECT_FALSE(pool.Release(first_slot));
  EXPECT_EQ(pool.Available(), 1U);
}
