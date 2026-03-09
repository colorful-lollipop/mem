#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/protocol.h"
#include "memrpc/compat/scan_behavior_codec.h"

TEST(ScanBehaviorCodecTest, RoundTripsScanBehaviorRequest) {
  memrpc::ScanBehaviorRequest request;
  request.behavior_text = "child process writes startup registry";

  std::vector<uint8_t> bytes;
  EXPECT_TRUE(memrpc::EncodeScanBehaviorRequest(request, &bytes));

  memrpc::ScanBehaviorRequest decoded;
  EXPECT_TRUE(memrpc::DecodeScanBehaviorRequest(bytes, &decoded));
  EXPECT_EQ(decoded.behavior_text, request.behavior_text);
}

TEST(ScanBehaviorCodecTest, RoundTripsScanBehaviorResult) {
  memrpc::ScanBehaviorResult result;
  result.status = memrpc::StatusCode::Ok;
  result.verdict = memrpc::ScanVerdict::Infected;
  result.engine_code = 31;
  result.detail_code = 41;
  result.message = "suspicious behavior";

  std::vector<uint8_t> bytes;
  EXPECT_TRUE(memrpc::EncodeScanBehaviorResult(result, &bytes));

  memrpc::ScanBehaviorResult decoded;
  EXPECT_TRUE(memrpc::DecodeScanBehaviorResult(bytes, &decoded));
  EXPECT_EQ(decoded.status, result.status);
  EXPECT_EQ(decoded.verdict, result.verdict);
  EXPECT_EQ(decoded.engine_code, result.engine_code);
  EXPECT_EQ(decoded.detail_code, result.detail_code);
  EXPECT_EQ(decoded.message, result.message);
}

TEST(ScanBehaviorCodecTest, RejectsOversizedBehaviorText) {
  memrpc::ScanBehaviorRequest request;
  request.behavior_text.assign(memrpc::kDefaultMaxRequestBytes + 1u, 'b');

  std::vector<uint8_t> bytes;
  EXPECT_FALSE(memrpc::EncodeScanBehaviorRequest(request, &bytes));
}
