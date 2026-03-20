#include <gtest/gtest.h>

#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

using VirusExecutorService::ScanTask;
using VirusExecutorService::ScanFileReply;

TEST(VesDemoCodecTest, ScanTaskRoundTrip) {
    ScanTask req;
    req.path = "/data/scan/clean.apk";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(MemRpc::CodecTraits<ScanTask>::Encode(req, &bytes));

    ScanTask decoded;
    ASSERT_TRUE(MemRpc::CodecTraits<ScanTask>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.path, req.path);
}

TEST(VesDemoCodecTest, ScanFileReplyRoundTrip) {
    ScanFileReply reply;
    reply.code = 0;
    reply.threatLevel = 1;

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(MemRpc::CodecTraits<ScanFileReply>::Encode(reply, &bytes));

    ScanFileReply decoded;
    ASSERT_TRUE(MemRpc::CodecTraits<ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.code, reply.code);
    EXPECT_EQ(decoded.threatLevel, reply.threatLevel);
}

TEST(VesDemoCodecTest, DecodeRejectsTruncatedPayload) {
    // Valid reply encoding is 8 bytes. Provide 4 bytes to force Decode failure.
    std::vector<uint8_t> truncated(4, 0);
    ScanFileReply decoded;
    EXPECT_FALSE(MemRpc::CodecTraits<ScanFileReply>::Decode(truncated.data(), truncated.size(), &decoded));
}

TEST(VesDemoCodecTest, NormalizeVesEngineKindsSortsAndDeduplicates) {
    std::vector<uint32_t> engineKinds = {
        99u,
        static_cast<uint32_t>(VesEngineKind::Scan),
        99u,
        7u,
    };

    EXPECT_EQ(NormalizeVesEngineKinds(std::move(engineKinds)),
              (std::vector<uint32_t>{
                  static_cast<uint32_t>(VesEngineKind::Scan),
                  7u,
                  99u,
              }));
}

TEST(VesDemoCodecTest, IsValidVesOpenSessionRequestRejectsZeroAndOversizedKinds) {
    VesOpenSessionRequest request;
    request.engineKinds = {static_cast<uint32_t>(VesEngineKind::Scan), 0u};
    EXPECT_FALSE(IsValidVesOpenSessionRequest(request));

    request.engineKinds.assign(VES_OPEN_SESSION_MAX_ENGINE_KINDS + 1, 1u);
    EXPECT_FALSE(IsValidVesOpenSessionRequest(request));

    request.engineKinds = {static_cast<uint32_t>(VesEngineKind::Scan), 7u};
    EXPECT_TRUE(IsValidVesOpenSessionRequest(request));
}

}  // namespace VirusExecutorService
