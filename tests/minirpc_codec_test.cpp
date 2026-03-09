#include <gtest/gtest.h>

#include <vector>

#include "apps/minirpc/common/minirpc_codec.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

TEST(MiniRpcCodecTest, EchoRoundTrips) {
  EchoRequest request;
  request.text = "hello";

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeEchoRequest(request, &bytes));

  EchoRequest decoded;
  ASSERT_TRUE(DecodeEchoRequest(bytes, &decoded));
  EXPECT_EQ(decoded.text, request.text);
}

TEST(MiniRpcCodecTest, AddRoundTrips) {
  AddRequest request;
  request.lhs = 7;
  request.rhs = 11;

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeAddRequest(request, &bytes));

  AddRequest decoded;
  ASSERT_TRUE(DecodeAddRequest(bytes, &decoded));
  EXPECT_EQ(decoded.lhs, request.lhs);
  EXPECT_EQ(decoded.rhs, request.rhs);
}

TEST(MiniRpcCodecTest, SleepRoundTrips) {
  SleepRequest sleep;
  sleep.delay_ms = 250;

  std::vector<uint8_t> sleep_bytes;
  ASSERT_TRUE(EncodeSleepRequest(sleep, &sleep_bytes));

  SleepRequest decoded_sleep;
  ASSERT_TRUE(DecodeSleepRequest(sleep_bytes, &decoded_sleep));
  EXPECT_EQ(decoded_sleep.delay_ms, sleep.delay_ms);
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
