#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "apps/vps/common/vps_codec.h"
#include "memrpc/server/handler.h"

namespace OHOS::Security::VirusProtectionService {
namespace {

TEST(VpsCodecTest, PollBehaviorReplyRoundTripsFromPayloadView) {
  PollBehaviorEventReply reply;
  reply.result = SUCCESS;
  reply.accessToken = 77u;
  reply.scanResult.eventId = "event-77";
  reply.scanResult.time = "2026-03-10T00:00:00Z";
  reply.scanResult.ruleName = "startup_persist";
  reply.scanResult.bundleName = "bundle";

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodePollBehaviorEventReply(reply, &bytes));

  PollBehaviorEventReply decoded;
  ASSERT_TRUE(DecodePollBehaviorEventReply(memrpc::PayloadView(bytes.data(), bytes.size()), &decoded));
  EXPECT_EQ(decoded.result, reply.result);
  EXPECT_EQ(decoded.accessToken, reply.accessToken);
  EXPECT_EQ(decoded.scanResult.eventId, reply.scanResult.eventId);
  EXPECT_EQ(decoded.scanResult.ruleName, reply.scanResult.ruleName);
}

TEST(VpsCodecTest, ScanBehaviorRequestRoundTripsFromPayloadView) {
  ScanBehaviorRequest request;
  request.accessToken = 99u;
  request.event = "startup";
  request.bundleName = "bundle.name";

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeScanBehaviorRequest(request, &bytes));

  ScanBehaviorRequest decoded;
  ASSERT_TRUE(DecodeScanBehaviorRequest(memrpc::PayloadView(bytes.data(), bytes.size()), &decoded));
  EXPECT_EQ(decoded.accessToken, request.accessToken);
  EXPECT_EQ(decoded.event, request.event);
  EXPECT_EQ(decoded.bundleName, request.bundleName);
}

TEST(VpsCodecTest, ScanFileReplyRoundTripsWithoutEnvelopeCopies) {
  ScanResult scan_result;
  scan_result.bundleName = "bundle";
  scan_result.bakPath = "/tmp/bak";
  scan_result.threatLevel = ThreatLevel::HIGH_RISK;
  scan_result.scanTaskType = ScanTaskType::DEEP;
  scan_result.accountId = 42;
  scan_result.engineResults.push_back(EngineResult{
      .virusName = "v",
      .virusType = "type",
      .errorMsg = "",
      .errorCode = 0,
      .level = ThreatLevel::HIGH_RISK,
  });

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeScanFileReply(SUCCESS, scan_result, &bytes));

  int32_t result_code = FAILED;
  ScanResult decoded;
  ASSERT_TRUE(DecodeScanFileReply(memrpc::PayloadView(bytes.data(), bytes.size()), &result_code,
                                  &decoded));
  EXPECT_EQ(result_code, SUCCESS);
  EXPECT_EQ(decoded.bundleName, scan_result.bundleName);
  EXPECT_EQ(decoded.engineResults.size(), scan_result.engineResults.size());
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService
