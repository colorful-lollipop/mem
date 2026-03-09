#include <gtest/gtest.h>

#include "memrpc/types.h"

TEST(TypesTest, ScanOptionsDefaultsToNormalPriority) {
  const memrpc::ScanOptions options;
  EXPECT_EQ(options.priority, memrpc::Priority::Normal);
  EXPECT_EQ(options.queue_timeout_ms, 1000u);
  EXPECT_EQ(options.exec_timeout_ms, 30000u);
}

TEST(TypesTest, ScanResultDefaultsAreStable) {
  const memrpc::ScanResult result;
  EXPECT_EQ(result.status, memrpc::StatusCode::Ok);
  EXPECT_EQ(result.verdict, memrpc::ScanVerdict::Unknown);
  EXPECT_EQ(result.engine_code, 0);
  EXPECT_EQ(result.detail_code, 0);
  EXPECT_TRUE(result.message.empty());
}

TEST(TypesTest, ScanBehaviorResultDefaultsAreStable) {
  const memrpc::ScanBehaviorResult result;
  EXPECT_EQ(result.status, memrpc::StatusCode::Ok);
  EXPECT_EQ(result.verdict, memrpc::ScanVerdict::Unknown);
  EXPECT_EQ(result.engine_code, 0);
  EXPECT_EQ(result.detail_code, 0);
  EXPECT_TRUE(result.message.empty());
}

TEST(TypesTest, StatusEnumValuesAreStable) {
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::QueueFull), 1);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::QueueTimeout), 2);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::ExecTimeout), 3);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::PeerDisconnected), 4);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::ProtocolMismatch), 5);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::EngineInternalError), 6);
  EXPECT_EQ(static_cast<int>(memrpc::StatusCode::InvalidArgument), 7);
}
