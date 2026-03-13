#include <gtest/gtest.h>

#include "vpsdemo/ves/ves_codec.h"
#include "vpsdemo/ves/ves_types.h"

namespace vpsdemo {

TEST(VesDemoCodecTest, ScanFileRequestRoundTrip) {
    ScanFileRequest req;
    req.filePath = "/data/scan/clean.apk";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileRequest>::Encode(req, &bytes));

    ScanFileRequest decoded;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileRequest>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.filePath, req.filePath);
}

TEST(VesDemoCodecTest, ScanFileReplyRoundTrip) {
    ScanFileReply reply;
    reply.code = 0;
    reply.threatLevel = 1;

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileReply>::Encode(reply, &bytes));

    ScanFileReply decoded;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.code, reply.code);
    EXPECT_EQ(decoded.threatLevel, reply.threatLevel);
}

TEST(VesDemoCodecTest, DecodeRejectsTruncatedPayload) {
    // Valid reply encoding is 8 bytes. Provide 4 bytes to force Decode failure.
    std::vector<uint8_t> truncated(4, 0);
    ScanFileReply decoded;
    EXPECT_FALSE(memrpc::CodecTraits<ScanFileReply>::Decode(truncated.data(), truncated.size(), &decoded));
}

}  // namespace vpsdemo
