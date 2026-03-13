#include <gtest/gtest.h>

#include "vpsdemo/testkit/testkit_codec.h"
#include "vpsdemo/testkit/testkit_types.h"

namespace vpsdemo::testkit {

TEST(TestkitCodecTest, EchoRequestRoundTrip) {
    EchoRequest request;
    request.text = "hello";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<EchoRequest>::Encode(request, &bytes));

    EchoRequest decoded;
    ASSERT_TRUE(memrpc::CodecTraits<EchoRequest>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.text, request.text);
}

TEST(TestkitCodecTest, AddReplyRoundTrip) {
    AddReply reply;
    reply.sum = 42;

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<AddReply>::Encode(reply, &bytes));

    AddReply decoded;
    ASSERT_TRUE(memrpc::CodecTraits<AddReply>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.sum, reply.sum);
}

TEST(TestkitCodecTest, SleepDecodeRejectsTruncatedPayload) {
    std::vector<uint8_t> truncated(2, 0);
    SleepReply decoded;
    EXPECT_FALSE(memrpc::CodecTraits<SleepReply>::Decode(
        truncated.data(), truncated.size(), &decoded));
}

}  // namespace vpsdemo::testkit
