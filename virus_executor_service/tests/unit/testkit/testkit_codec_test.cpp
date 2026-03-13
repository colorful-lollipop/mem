#include <gtest/gtest.h>

#include "testkit/testkit_codec.h"
#include "testkit/testkit_types.h"

namespace virus_executor_service::testkit {

TEST(TestkitCodecTest, EchoRequestRoundTrip) {
    EchoRequest request;
    request.text = "hello";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(MemRpc::CodecTraits<EchoRequest>::Encode(request, &bytes));

    EchoRequest decoded;
    ASSERT_TRUE(MemRpc::CodecTraits<EchoRequest>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.text, request.text);
}

TEST(TestkitCodecTest, AddReplyRoundTrip) {
    AddReply reply;
    reply.sum = 42;

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(MemRpc::CodecTraits<AddReply>::Encode(reply, &bytes));

    AddReply decoded;
    ASSERT_TRUE(MemRpc::CodecTraits<AddReply>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.sum, reply.sum);
}

TEST(TestkitCodecTest, SleepDecodeRejectsTruncatedPayload) {
    std::vector<uint8_t> truncated(2, 0);
    SleepReply decoded;
    EXPECT_FALSE(MemRpc::CodecTraits<SleepReply>::Decode(
        truncated.data(), truncated.size(), &decoded));
}

}  // namespace virus_executor_service::testkit
