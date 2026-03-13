#include <gtest/gtest.h>

#include "memrpc/client/rpc_client.h"

TEST(RpcClientRecoveryPolicyTest, PolicyCanBeSet) {
  memrpc::RpcClient client;
  memrpc::RecoveryPolicy policy;
  policy.onFailure = [](const memrpc::RpcFailure&) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, AllHandlersCanBeSet) {
  memrpc::RpcClient client;
  memrpc::RecoveryPolicy policy;
  policy.onFailure = [](const memrpc::RpcFailure&) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
  };
  policy.onIdle = [](uint64_t) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
  };
  policy.onEngineDeath = [](const memrpc::EngineDeathReport&) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, 100};
  };
  policy.idleTimeoutMs = 1000;
  policy.idleNotifyIntervalMs = 500;
  client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, RecoveryActionEnum) {
  EXPECT_NE(memrpc::RecoveryAction::Ignore, memrpc::RecoveryAction::Restart);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryDecisionDefaults) {
  memrpc::RecoveryDecision decision;
  EXPECT_EQ(decision.action, memrpc::RecoveryAction::Ignore);
  EXPECT_EQ(decision.delayMs, 0u);
}

TEST(RpcClientRecoveryPolicyTest, SyncClientPolicyCanBeSet) {
  memrpc::RpcSyncClient client;
  memrpc::RecoveryPolicy policy;
  policy.onEngineDeath = [](const memrpc::EngineDeathReport&) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, 200};
  };
  client.SetRecoveryPolicy(std::move(policy));
}
