#include <gtest/gtest.h>

#include "memrpc/core/types.h"

TEST(TypesTest, PriorityEnumValuesAreStable)
{
    EXPECT_EQ(static_cast<int>(MemRpc::Priority::Normal), 0);
    EXPECT_EQ(static_cast<int>(MemRpc::Priority::High), 1);
}

TEST(TypesTest, StatusEnumValuesAreStable)
{
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::QueueFull), 1);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::QueueTimeout), 2);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::ExecTimeout), 3);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::PeerDisconnected), 4);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::ProtocolMismatch), 5);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::EngineInternalError), 6);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::InvalidArgument), 7);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::CrashedDuringExecution), 8);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::CooldownActive), 9);
    EXPECT_EQ(static_cast<int>(MemRpc::StatusCode::ClientClosed), 10);
}
