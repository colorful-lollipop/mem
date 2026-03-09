#include <gtest/gtest.h>

#include "core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(memrpc::kSharedMemoryMagic, 0x4d454d52u);
  EXPECT_EQ(memrpc::kProtocolVersion, 1u);
  EXPECT_EQ(memrpc::kDefaultMaxRequestBytes, 16u * 1024u);
  EXPECT_EQ(memrpc::kDefaultMaxResponseBytes, 1024u);
  EXPECT_EQ(sizeof(memrpc::RequestRingEntry), 32u);
  EXPECT_GE(sizeof(memrpc::ResponseRingEntry), 24u);
}

TEST(ProtocolLayoutTest, GenericSlotPayloadCarriesRequestAndResponseAreas) {
  EXPECT_EQ(memrpc::ComputeSlotSize(memrpc::kDefaultMaxRequestBytes,
                                    memrpc::kDefaultMaxResponseBytes),
            sizeof(memrpc::SlotPayload) + memrpc::kDefaultMaxRequestBytes +
                memrpc::kDefaultMaxResponseBytes);
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
  EXPECT_LT(layout.slot_pool_offset, layout.total_size);
}

TEST(ProtocolLayoutTest, ResponseRingEntryDistinguishesReplyAndEventMessages) {
  memrpc::ResponseRingEntry reply;
  reply.message_kind = memrpc::ResponseMessageKind::Reply;
  reply.request_id = 123;

  memrpc::ResponseRingEntry event;
  event.message_kind = memrpc::ResponseMessageKind::Event;
  event.event_domain = 7;
  event.event_type = 9;
  event.flags = 11;

  EXPECT_NE(reply.message_kind, event.message_kind);
  EXPECT_EQ(event.request_id, 0u);
}
