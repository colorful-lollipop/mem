#include <gtest/gtest.h>

#include "memrpc/core/types.h"

TEST(TypesTest, ScanOptionsDefaultsToNormalPriority) {
  const MemRpc::ScanOptions options;
  EXPECT_EQ(options.priority, MemRpc::Priority::Normal);
  EXPECT_EQ(options.queueTimeoutMs, 1000u);
  EXPECT_EQ(options.execTimeoutMs, 30000u);
}

TEST(TypesTest, ScanResultDefaultsAreStable) {
  const MemRpc::ScanResult result;
  EXPECT_EQ(result.status, MemRpc::StatusCode::Ok);
  EXPECT_EQ(result.verdict, MemRpc::ScanVerdict::Unknown);
  EXPECT_EQ(result.engineCode, 0);
  EXPECT_EQ(result.detailCode, 0);
  EXPECT_TRUE(result.message.empty());
}

TEST(TypesTest, ScanBehaviorResultDefaultsAreStable) {
  const MemRpc::ScanBehaviorResult result;
  EXPECT_EQ(result.status, MemRpc::StatusCode::Ok);
  EXPECT_EQ(result.verdict, MemRpc::ScanVerdict::Unknown);
  EXPECT_EQ(result.engineCode, 0);
  EXPECT_EQ(result.detailCode, 0);
  EXPECT_TRUE(result.message.empty());
}

TEST(TypesTest, StatusEnumValuesAreStable) {
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::QueueFull), 1);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::QueueTimeout), 2);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::ExecTimeout), 3);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::PeerDisconnected), 4);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::ProtocolMismatch), 5);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::EngineInternalError), 6);
  EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::InvalidArgument), 7);
}
