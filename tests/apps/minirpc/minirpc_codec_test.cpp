#include <gtest/gtest.h>

#include <vector>

#include "apps/minirpc/common/minirpc_codec.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

TEST(MiniRpcCodecTest, EchoRoundTrips) {
  EchoRequest request;
  request.text = "hello";

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeMessage<EchoRequest>(request, &bytes));

  EchoRequest decoded;
  ASSERT_TRUE(DecodeMessage<EchoRequest>(bytes, &decoded));
  EXPECT_EQ(decoded.text, request.text);
}

TEST(MiniRpcCodecTest, AddRoundTrips) {
  AddRequest request;
  request.lhs = 7;
  request.rhs = 11;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeMessage<AddRequest>(request, &bytes));

  AddRequest decoded;
  ASSERT_TRUE(DecodeMessage<AddRequest>(bytes, &decoded));
  EXPECT_EQ(decoded.lhs, request.lhs);
  EXPECT_EQ(decoded.rhs, request.rhs);
}

TEST(MiniRpcCodecTest, SleepRoundTrips) {
  SleepRequest sleep;
  sleep.delay_ms = 250;

  std::vector<uint8_t> sleep_bytes;
  ASSERT_TRUE(EncodeMessage<SleepRequest>(sleep, &sleep_bytes));

  SleepRequest decoded_sleep;
  ASSERT_TRUE(DecodeMessage<SleepRequest>(sleep_bytes, &decoded_sleep));
  EXPECT_EQ(decoded_sleep.delay_ms, sleep.delay_ms);
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
