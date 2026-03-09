#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/protocol.h"
#include "memrpc/compat/scan_codec.h"

TEST(ScanCodecTest, RoundTripsScanRequest) {
  memrpc::ScanRequest request;
  request.file_path = "/tmp/file";

  std::vector<uint8_t> bytes;
  EXPECT_TRUE(memrpc::EncodeScanRequest(request, &bytes));

  memrpc::ScanRequest decoded;
  EXPECT_TRUE(memrpc::DecodeScanRequest(bytes, &decoded));
  EXPECT_EQ(decoded.file_path, request.file_path);
}

TEST(ScanCodecTest, RoundTripsScanResult) {
  memrpc::ScanResult result;
  result.status = memrpc::StatusCode::Ok;
  result.verdict = memrpc::ScanVerdict::Infected;
  result.engine_code = 11;
  result.detail_code = 22;
  result.message = "infected";

  std::vector<uint8_t> bytes;
  EXPECT_TRUE(memrpc::EncodeScanResult(result, &bytes));

  memrpc::ScanResult decoded;
  EXPECT_TRUE(memrpc::DecodeScanResult(bytes, &decoded));
  EXPECT_EQ(decoded.status, result.status);
  EXPECT_EQ(decoded.verdict, result.verdict);
  EXPECT_EQ(decoded.engine_code, result.engine_code);
  EXPECT_EQ(decoded.detail_code, result.detail_code);
  EXPECT_EQ(decoded.message, result.message);
}

TEST(ScanCodecTest, RejectsOversizedStrings) {
  memrpc::ScanRequest request;
  request.file_path.assign(memrpc::kDefaultMaxRequestBytes + 1u, 'a');

  std::vector<uint8_t> bytes;
  EXPECT_FALSE(memrpc::EncodeScanRequest(request, &bytes));
}
