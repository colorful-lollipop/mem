#include <gtest/gtest.h>

#include "memrpc/types.h"

TEST(TypesTest, ScanOptionsDefaultsToNormalPriority) {
  const memrpc::ScanOptions options;
  EXPECT_EQ(options.priority, memrpc::Priority::kNormal);
  EXPECT_EQ(options.queue_timeout_ms, 1000u);
  EXPECT_EQ(options.exec_timeout_ms, 30000u);
}

TEST(TypesTest, ScanResultDefaultsAreStable) {
  const memrpc::ScanResult result;
  EXPECT_EQ(result.status, memrpc::StatusCode::kOk);
  EXPECT_EQ(result.verdict, memrpc::ScanVerdict::kUnknown);
  EXPECT_EQ(result.engine_code, 0);
  EXPECT_EQ(result.detail_code, 0);
  EXPECT_TRUE(result.message.empty());
}

TEST(TypesTest, StatusEnumValuesAreStable) {
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kQueueFull), 1);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kQueueTimeout), 2);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kExecTimeout), 3);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kPeerDisconnected), 4);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kProtocolMismatch), 5);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kEngineInternalError), 6);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::kInvalidArgument), 7);
}
