#include <gtest/gtest.h>

#include <cstddef>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(MemRpc::SHARED_MEMORY_MAGIC, 0x4d454d52U);
  EXPECT_EQ(MemRpc::PROTOCOL_VERSION, 3U);
  EXPECT_EQ(MemRpc::DEFAULT_MAX_REQUEST_BYTES, 4U * 1024U);
  EXPECT_EQ(MemRpc::DEFAULT_MAX_RESPONSE_BYTES, 4U * 1024U);
  EXPECT_EQ(sizeof(MemRpc::RequestRingEntry), 32U);
  EXPECT_EQ(sizeof(MemRpc::SlotRuntimeState), 32U);
  EXPECT_LE(sizeof(MemRpc::ResponseRingEntry), 64U);
}

TEST(ProtocolLayoutTest, SlotSizeOnlyDependsOnRequestArea) {
  EXPECT_EQ(MemRpc::ComputeSlotSize(MemRpc::DEFAULT_MAX_REQUEST_BYTES,
                                    MemRpc::DEFAULT_MAX_RESPONSE_BYTES),
            sizeof(MemRpc::SlotPayload) + MemRpc::DEFAULT_MAX_REQUEST_BYTES);
  EXPECT_EQ(MemRpc::ComputeSlotSize(4096U, 256U), MemRpc::ComputeSlotSize(4096U, 1024U));
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Free), 0U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Admitted), 1U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Queued), 2U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Executing), 3U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Responding), 4U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Ready), 5U);
  EXPECT_EQ(static_cast<uint32_t>(MemRpc::SlotRuntimeStateCode::Consumed), 6U);
}

TEST(ProtocolLayoutTest, DemoBootstrapDefaultsAreSizedForSmallSessions) {
  MemRpc::DemoBootstrapConfig config;
  EXPECT_EQ(config.highRingSize, 32U);
  EXPECT_EQ(config.normalRingSize, 32U);
  EXPECT_EQ(config.responseRingSize, 64U);
  EXPECT_EQ(config.slotCount, 64U);
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
  EXPECT_EQ(sizeof(MemRpc::RingCursor), 16U);
  MemRpc::RingCursor cursor;
  cursor.head.store(2, std::memory_order_relaxed);
  cursor.tail.store(5, std::memory_order_relaxed);
  cursor.capacity = 8;
  EXPECT_EQ(MemRpc::RingCount(cursor), 3U);
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
  EXPECT_EQ(event.requestId, 0U);
  EXPECT_EQ(reply.slotIndex, 5U);
  EXPECT_EQ(event.slotIndex, 7U);
}
