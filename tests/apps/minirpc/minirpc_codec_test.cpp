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

  EchoRequestView decoded_view;
  ASSERT_TRUE(DecodeMessageView<EchoRequestView>(bytes, &decoded_view));
  EXPECT_EQ(decoded_view.text, request.text);
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

TEST(MiniRpcCodecTest, AllViewsDecodeRoundTrip) {
  std::vector<uint8_t> bytes;

  EchoRequest echo_request;
  echo_request.text = "hello";
  ASSERT_TRUE(EncodeMessage<EchoRequest>(echo_request, &bytes));
  EchoRequestView echo_request_view;
  ASSERT_TRUE(DecodeMessageView<EchoRequestView>(bytes, &echo_request_view));
  EXPECT_EQ(echo_request_view.text, echo_request.text);

  EchoReply echo_reply;
  echo_reply.text = "world";
  ASSERT_TRUE(EncodeMessage<EchoReply>(echo_reply, &bytes));
  EchoReplyView echo_reply_view;
  ASSERT_TRUE(DecodeMessageView<EchoReplyView>(bytes, &echo_reply_view));
  EXPECT_EQ(echo_reply_view.text, echo_reply.text);

  AddRequest add_request;
  add_request.lhs = 1;
  add_request.rhs = 2;
  ASSERT_TRUE(EncodeMessage<AddRequest>(add_request, &bytes));
  AddRequestView add_request_view;
  ASSERT_TRUE(DecodeMessageView<AddRequestView>(bytes, &add_request_view));
  EXPECT_EQ(add_request_view.lhs, add_request.lhs);
  EXPECT_EQ(add_request_view.rhs, add_request.rhs);

  AddReply add_reply;
  add_reply.sum = 3;
  ASSERT_TRUE(EncodeMessage<AddReply>(add_reply, &bytes));
  AddReplyView add_reply_view;
  ASSERT_TRUE(DecodeMessageView<AddReplyView>(bytes, &add_reply_view));
  EXPECT_EQ(add_reply_view.sum, add_reply.sum);

  SleepRequest sleep_request;
  sleep_request.delay_ms = 250;
  ASSERT_TRUE(EncodeMessage<SleepRequest>(sleep_request, &bytes));
  SleepRequestView sleep_request_view;
  ASSERT_TRUE(DecodeMessageView<SleepRequestView>(bytes, &sleep_request_view));
  EXPECT_EQ(sleep_request_view.delay_ms, sleep_request.delay_ms);

  SleepReply sleep_reply;
  sleep_reply.status = 0;
  ASSERT_TRUE(EncodeMessage<SleepReply>(sleep_reply, &bytes));
  SleepReplyView sleep_reply_view;
  ASSERT_TRUE(DecodeMessageView<SleepReplyView>(bytes, &sleep_reply_view));
  EXPECT_EQ(sleep_reply_view.status, sleep_reply.status);
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
