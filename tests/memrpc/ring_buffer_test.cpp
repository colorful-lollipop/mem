#include <array>

#include <gtest/gtest.h>

#include "core/ring_buffer.h"

TEST(RingBufferTest, PushPopOneEntry) {
  std::array<int, 2> storage{};
  memrpc::RingBuffer<int> ring(storage.data(), storage.size());

  EXPECT_TRUE(ring.Empty());
  EXPECT_EQ(ring.Size(), 0u);
  EXPECT_TRUE(ring.Push(7));
  EXPECT_FALSE(ring.Empty());
  EXPECT_EQ(ring.Size(), 1u);

  int value = 0;
  EXPECT_TRUE(ring.Pop(&value));
  EXPECT_EQ(value, 7);
  EXPECT_TRUE(ring.Empty());
  EXPECT_EQ(ring.Size(), 0u);
}

TEST(RingBufferTest, FullRingRejectsPush) {
  std::array<int, 2> storage{};
  memrpc::RingBuffer<int> ring(storage.data(), storage.size());

  EXPECT_TRUE(ring.Push(1));
  EXPECT_TRUE(ring.Push(2));
  EXPECT_TRUE(ring.Full());
  EXPECT_FALSE(ring.Push(3));
  EXPECT_EQ(ring.Size(), 2u);
}

TEST(RingBufferTest, PreservesFifoOrdering) {
  std::array<int, 3> storage{};
  memrpc::RingBuffer<int> ring(storage.data(), storage.size());

  EXPECT_TRUE(ring.Push(10));
  EXPECT_TRUE(ring.Push(20));
  EXPECT_TRUE(ring.Push(30));

  int first = 0;
  int second = 0;
  int third = 0;
  EXPECT_TRUE(ring.Pop(&first));
  EXPECT_TRUE(ring.Pop(&second));
  EXPECT_TRUE(ring.Pop(&third));
  EXPECT_EQ(first, 10);
  EXPECT_EQ(second, 20);
  EXPECT_EQ(third, 30);
}
