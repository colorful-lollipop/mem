#include <gtest/gtest.h>

#include "core/protocol.h"
#include "core/shm_layout.h"

TEST(ProtocolLayoutTest, ConstantsAndEntrySizesAreStable) {
  EXPECT_EQ(memrpc::kSharedMemoryMagic, 0x4d454d52u);
  EXPECT_EQ(memrpc::kProtocolVersion, 1u);
  EXPECT_EQ(sizeof(memrpc::RequestRingEntry), 32u);
  EXPECT_EQ(sizeof(memrpc::ResponseRingEntry), 24u);
}

TEST(ProtocolLayoutTest, OffsetsIncreaseMonotonically) {
  const memrpc::LayoutConfig config{
      8,
      16,
      32,
      64,
  };
  const memrpc::Layout layout = memrpc::ComputeLayout(config);
  EXPECT_LT(layout.high_ring_offset, layout.normal_ring_offset);
  EXPECT_LT(layout.normal_ring_offset, layout.response_ring_offset);
  EXPECT_LT(layout.response_ring_offset, layout.slot_pool_offset);
  EXPECT_LT(layout.slot_pool_offset, layout.total_size);
}
