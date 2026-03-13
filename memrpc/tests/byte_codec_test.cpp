#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "memrpc/core/byte_reader.h"
#include "memrpc/core/byte_writer.h"

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

TEST(ByteCodecTest, ReadsStringAndBytesAsViews) {
  memrpc::ByteWriter writer;
  const std::string text = "hello";
  const std::vector<uint8_t> bytes = {9u, 8u, 7u};
  ASSERT_TRUE(writer.WriteString(text));
  ASSERT_TRUE(writer.WriteUint32(static_cast<uint32_t>(bytes.size())));
  ASSERT_TRUE(writer.WriteBytes(bytes.data(), static_cast<uint32_t>(bytes.size())));

  memrpc::ByteReader reader(writer.bytes());
  std::string_view text_view;
  memrpc::ByteView bytes_view;
  ASSERT_TRUE(reader.ReadStringView(&text_view));
  uint32_t size = 0;
  ASSERT_TRUE(reader.ReadUint32(&size));
  ASSERT_TRUE(reader.ReadBytesView(size, &bytes_view));
  EXPECT_EQ(text_view, "hello");
  EXPECT_EQ(bytes_view.size(), bytes.size());
  EXPECT_EQ(bytes_view[0], static_cast<uint8_t>(9u));
  EXPECT_EQ(bytes_view[2], static_cast<uint8_t>(7u));
}
