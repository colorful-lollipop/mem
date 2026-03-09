#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/byte_reader.h"
#include "core/byte_writer.h"

TEST(ByteCodecTest, RoundTripsIntegersAndStrings) {
  memrpc::ByteWriter writer;
  EXPECT_TRUE(writer.WriteUint32(7u));
  EXPECT_TRUE(writer.WriteInt32(-9));
  EXPECT_TRUE(writer.WriteString("hello"));

  memrpc::ByteReader reader(writer.bytes());
  uint32_t value_u32 = 0;
  int32_t value_i32 = 0;
  std::string text;
  EXPECT_TRUE(reader.ReadUint32(&value_u32));
  EXPECT_TRUE(reader.ReadInt32(&value_i32));
  EXPECT_TRUE(reader.ReadString(&text));
  EXPECT_EQ(value_u32, 7u);
  EXPECT_EQ(value_i32, -9);
  EXPECT_EQ(text, "hello");
}

TEST(ByteCodecTest, FailsOnOutOfBoundsReads) {
  const std::vector<uint8_t> bytes = {1u, 2u, 3u};
  memrpc::ByteReader reader(bytes);
  uint32_t value = 0;
  EXPECT_FALSE(reader.ReadUint32(&value));
}
