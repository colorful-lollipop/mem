#include <gtest/gtest.h>

#include <cstddef>

#include "core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(memrpc::kSharedMemoryMagic, 0x4d454d52u);
  EXPECT_EQ(memrpc::kProtocolVersion, 2u);
  EXPECT_EQ(memrpc::kDefaultMaxRequestBytes, 4u * 1024u);
  EXPECT_EQ(memrpc::kDefaultMaxResponseBytes, 4u * 1024u);
  EXPECT_EQ(sizeof(memrpc::RequestRingEntry), 32u);
  EXPECT_EQ(sizeof(memrpc::SlotRuntimeState), 32u);
  EXPECT_LE(sizeof(memrpc::ResponseRingEntry), 64u);
}

TEST(ProtocolLayoutTest, SlotSizeOnlyDependsOnRequestArea) {
  EXPECT_EQ(memrpc::ComputeSlotSize(memrpc::kDefaultMaxRequestBytes,
                                    memrpc::kDefaultMaxResponseBytes),
            sizeof(memrpc::SlotPayload) + memrpc::kDefaultMaxRequestBytes);
  EXPECT_EQ(memrpc::ComputeSlotSize(4096u, 256u), memrpc::ComputeSlotSize(4096u, 1024u));
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Free), 0u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Admitted), 1u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Queued), 2u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Executing), 3u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Responding), 4u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Ready), 5u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Consumed), 6u);
}

TEST(ProtocolLayoutTest, OffsetsIncreaseMonotonically) {
  const memrpc::LayoutConfig config{
      8,
      16,
      32,
      64,
      sizeof(memrpc::SlotPayload),
      memrpc::kDefaultMaxRequestBytes,
      memrpc::kDefaultMaxResponseBytes,
  };
  const memrpc::Layout layout = memrpc::ComputeLayout(config);
  EXPECT_LT(layout.high_ring_offset, layout.normal_ring_offset);
  EXPECT_LT(layout.normal_ring_offset, layout.response_ring_offset);
  EXPECT_LT(layout.response_ring_offset, layout.slot_pool_offset);
  EXPECT_LT(layout.slot_pool_offset, layout.response_slot_pool_offset);
  EXPECT_LT(layout.response_slot_pool_offset, layout.response_slots_offset);
  EXPECT_LT(layout.response_slots_offset, layout.total_size);
}

TEST(ProtocolLayoutTest, RingCursorLayoutMatchesSpscHeadTailModel) {
  EXPECT_EQ(sizeof(memrpc::RingCursor), 16u);
  memrpc::RingCursor cursor;
  cursor.head.store(2, std::memory_order_relaxed);
  cursor.tail.store(5, std::memory_order_relaxed);
  cursor.capacity = 8;
  EXPECT_EQ(memrpc::RingCount(cursor), 3u);
}

TEST(ProtocolLayoutTest, ResponseRingEntryDistinguishesReplyAndEventMessages) {
  memrpc::ResponseRingEntry reply;
  reply.message_kind = memrpc::ResponseMessageKind::Reply;
  reply.request_id = 123;
  reply.slot_index = 5;
  reply.result_size = 3;

  memrpc::ResponseRingEntry event;
  event.message_kind = memrpc::ResponseMessageKind::Event;
  event.slot_index = 7;
  event.event_domain = 7;
  event.event_type = 9;
  event.flags = 11;
  event.result_size = 2;

  EXPECT_NE(reply.message_kind, event.message_kind);
  EXPECT_EQ(event.request_id, 0u);
  EXPECT_EQ(reply.slot_index, 5u);
  EXPECT_EQ(event.slot_index, 7u);
}
