#include <gtest/gtest.h>

#include <cstddef>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(MemRpc::SHARED_MEMORY_MAGIC, 0x4d454d52u);
  EXPECT_EQ(MemRpc::PROTOCOL_VERSION, 3u);
  EXPECT_EQ(MemRpc::DEFAULT_MAX_REQUEST_BYTES, 4u * 1024u);
  EXPECT_EQ(MemRpc::DEFAULT_MAX_RESPONSE_BYTES, 4u * 1024u);
  EXPECT_EQ(sizeof(MemRpc::RequestRingEntry), 32u);
  EXPECT_EQ(sizeof(MemRpc::SlotRuntimeState), 32u);
  EXPECT_LE(sizeof(MemRpc::ResponseRingEntry), 64u);
}

TEST(ProtocolLayoutTest, SlotSizeOnlyDependsOnRequestArea) {
  EXPECT_EQ(MemRpc::ComputeSlotSize(MemRpc::DEFAULT_MAX_REQUEST_BYTES,
                                    MemRpc::DEFAULT_MAX_RESPONSE_BYTES),
            sizeof(MemRpc::SlotPayload) + MemRpc::DEFAULT_MAX_REQUEST_BYTES);
  EXPECT_EQ(MemRpc::ComputeSlotSize(4096u, 256u), MemRpc::ComputeSlotSize(4096u, 1024u));
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Free), 0u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Admitted), 1u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Queued), 2u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Executing), 3u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Responding), 4u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Ready), 5u);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Consumed), 6u);
}

TEST(ProtocolLayoutTest, DemoBootstrapDefaultsAreSizedForSmallSessions) {
  MemRpc::DemoBootstrapConfig config;
  EXPECT_EQ(config.highRingSize, 32u);
  EXPECT_EQ(config.normalRingSize, 32u);
  EXPECT_EQ(config.responseRingSize, 64u);
  EXPECT_EQ(config.slotCount, 64u);
}

TEST(ProtocolLayoutTest, OffsetsIncreaseMonotonically) {
  const MemRpc::LayoutConfig config{
      8,
      16,
      32,
      64,
      sizeof(MemRpc::SlotPayload),
      MemRpc::DEFAULT_MAX_REQUEST_BYTES,
      MemRpc::DEFAULT_MAX_RESPONSE_BYTES,
  };
  const MemRpc::Layout layout = MemRpc::ComputeLayout(config);
  EXPECT_LT(layout.highRingOffset, layout.normalRingOffset);
  EXPECT_LT(layout.normalRingOffset, layout.responseRingOffset);
  EXPECT_LT(layout.responseRingOffset, layout.slotPoolOffset);
  EXPECT_LT(layout.slotPoolOffset, layout.responseSlotPoolOffset);
  EXPECT_LT(layout.responseSlotPoolOffset, layout.responseSlotsOffset);
  EXPECT_LT(layout.responseSlotsOffset, layout.totalSize);
}

TEST(ProtocolLayoutTest, RingCursorLayoutMatchesSpscHeadTailModel) {
  EXPECT_EQ(sizeof(MemRpc::RingCursor), 16u);
  MemRpc::RingCursor cursor;
  cursor.head.store(2, std::memory_order_relaxed);
  cursor.tail.store(5, std::memory_order_relaxed);
  cursor.capacity = 8;
  EXPECT_EQ(MemRpc::RingCount(cursor), 3u);
}

TEST(ProtocolLayoutTest, ResponseRingEntryDistinguishesReplyAndEventMessages) {
  MemRpc::ResponseRingEntry reply;
  reply.messageKind = MemRpc::ResponseMessageKind::Reply;
  reply.requestId = 123;
  reply.slotIndex = 5;
  reply.resultSize = 3;

  MemRpc::ResponseRingEntry event;
  event.messageKind = MemRpc::ResponseMessageKind::Event;
  event.slotIndex = 7;
  event.eventDomain = 7;
  event.eventType = 9;
  event.flags = 11;
  event.resultSize = 2;

  EXPECT_NE(reply.messageKind, event.messageKind);
  EXPECT_EQ(event.requestId, 0u);
  EXPECT_EQ(reply.slotIndex, 5u);
  EXPECT_EQ(event.slotIndex, 7u);
}
