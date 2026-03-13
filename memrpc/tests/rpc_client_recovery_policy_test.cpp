#include <gtest/gtest.h>

#include "memrpc/client/rpc_client.h"

TEST(RpcClientRecoveryPolicyTest, PolicyCanBeSet) {
  MemRpc::RpcClient client;
  MemRpc::RecoveryPolicy policy;
  policy.onFailure = [](const MemRpc::RpcFailure&) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, AllHandlersCanBeSet) {
  MemRpc::RpcClient client;
  MemRpc::RecoveryPolicy policy;
  policy.onFailure = [](const MemRpc::RpcFailure&) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  policy.onIdle = [](uint64_t) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 100};
  };
  policy.idleTimeoutMs = 1000;
  policy.idleNotifyIntervalMs = 500;
  client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, RecoveryActionEnum) {
  EXPECT_NE(MemRpc::RecoveryAction::Ignore, MemRpc::RecoveryAction::Restart);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryDecisionDefaults) {
  MemRpc::RecoveryDecision decision;
  EXPECT_EQ(decision.action, MemRpc::RecoveryAction::Ignore);
  EXPECT_EQ(decision.delayMs, 0u);
}

TEST(RpcClientRecoveryPolicyTest, SyncClientPolicyCanBeSet) {
  MemRpc::RpcSyncClient client;
  MemRpc::RecoveryPolicy policy;
  policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 200};
  };
  client.SetRecoveryPolicy(std::move(policy));
}
