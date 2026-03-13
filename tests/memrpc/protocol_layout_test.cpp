#include <gtest/gtest.h>

#include <cstddef>

#include "memrpc/client/demo_bootstrap.h"
#include "core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(memrpc::SHARED_MEMORY_MAGIC, 0x4d454d52u);
  EXPECT_EQ(memrpc::PROTOCOL_VERSION, 3u);
  EXPECT_EQ(memrpc::DEFAULT_MAX_REQUEST_BYTES, 4u * 1024u);
  EXPECT_EQ(memrpc::DEFAULT_MAX_RESPONSE_BYTES, 4u * 1024u);
  EXPECT_EQ(sizeof(memrpc::RequestRingEntry), 32u);
  EXPECT_EQ(sizeof(memrpc::SlotRuntimeState), 32u);
  EXPECT_LE(sizeof(memrpc::ResponseRingEntry), 64u);
}

TEST(ProtocolLayoutTest, SlotSizeOnlyDependsOnRequestArea) {
  EXPECT_EQ(memrpc::ComputeSlotSize(memrpc::DEFAULT_MAX_REQUEST_BYTES,
                                    memrpc::DEFAULT_MAX_RESPONSE_BYTES),
            sizeof(memrpc::SlotPayload) + memrpc::DEFAULT_MAX_REQUEST_BYTES);
  EXPECT_EQ(memrpc::ComputeSlotSize(4096u, 256u), memrpc::ComputeSlotSize(4096u, 1024u));
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Free), 0u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Admitted), 1u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Queued), 2u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Executing), 3u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Responding), 4u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Ready), 5u);
  EXPECT_EQ(static_cast<uint32_t>(memrpc::SlotRuntimeStateCode::Consumed), 6u);
}

TEST(ProtocolLayoutTest, DemoBootstrapDefaultsAreSizedForSmallSessions) {
  memrpc::DemoBootstrapConfig config;
  EXPECT_EQ(config.high_ring_size, 32u);
  EXPECT_EQ(config.normal_ring_size, 32u);
  EXPECT_EQ(config.response_ring_size, 64u);
  EXPECT_EQ(config.slot_count, 64u);
}

TEST(ProtocolLayoutTest, OffsetsIncreaseMonotonically) {
  const memrpc::LayoutConfig config{
      8,
      16,
      32,
      64,
      sizeof(memrpc::SlotPayload),
      memrpc::DEFAULT_MAX_REQUEST_BYTES,
      memrpc::DEFAULT_MAX_RESPONSE_BYTES,
  };
  const memrpc::Layout layout = memrpc::ComputeLayout(config);
  EXPECT_LT(layout.highRingOffset, layout.normalRingOffset);
  EXPECT_LT(layout.normalRingOffset, layout.responseRingOffset);
  EXPECT_LT(layout.responseRingOffset, layout.slotPoolOffset);
  EXPECT_LT(layout.slotPoolOffset, layout.responseSlotPoolOffset);
  EXPECT_LT(layout.responseSlotPoolOffset, layout.responseSlotsOffset);
  EXPECT_LT(layout.responseSlotsOffset, layout.totalSize);
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
