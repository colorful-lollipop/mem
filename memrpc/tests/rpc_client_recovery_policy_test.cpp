#include <gtest/gtest.h>

#include "memrpc/client/rpc_client.h"

TEST(RpcClientRecoveryPolicyTest, PolicyCanBeSet)
{
    MemRpc::RpcClient client;
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [](const MemRpc::RpcFailure&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, AllHandlersCanBeSet)
{
    MemRpc::RpcClient client;
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [](const MemRpc::RpcFailure&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    policy.onIdle = [](uint64_t) { return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0}; };
    policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 100};
    };
    client.SetRecoveryPolicy(std::move(policy));
}

TEST(RpcClientRecoveryPolicyTest, RecoveryActionEnum)
{
    EXPECT_NE(MemRpc::RecoveryAction::Ignore, MemRpc::RecoveryAction::Restart);
    EXPECT_NE(MemRpc::RecoveryAction::Restart, MemRpc::RecoveryAction::IdleClose);
    EXPECT_NE(MemRpc::RecoveryAction::IdleClose, MemRpc::RecoveryAction::ManualShutdown);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryDecisionDefaults)
{
    MemRpc::RecoveryDecision decision;
    EXPECT_EQ(decision.action, MemRpc::RecoveryAction::Ignore);
    EXPECT_EQ(decision.delayMs, 0u);
}

TEST(RpcClientRecoveryPolicyTest, LifecycleAndTriggerEnumsExposeUnifiedModel)
{
    EXPECT_NE(MemRpc::ClientLifecycleState::Uninitialized, MemRpc::ClientLifecycleState::Active);
    EXPECT_NE(MemRpc::ClientLifecycleState::IdleClosed, MemRpc::ClientLifecycleState::Closed);
    EXPECT_NE(MemRpc::RecoveryTrigger::ManualShutdown, MemRpc::RecoveryTrigger::IdlePolicy);
    EXPECT_NE(MemRpc::RecoveryTrigger::EngineDeath, MemRpc::RecoveryTrigger::DemandReconnect);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryRuntimeSnapshotDefaults)
{
    MemRpc::RecoveryRuntimeSnapshot snapshot;
    EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Uninitialized);
    EXPECT_EQ(snapshot.lastTrigger, MemRpc::RecoveryTrigger::Unknown);
    EXPECT_EQ(snapshot.lastRecoveryAction, MemRpc::RecoveryAction::Ignore);
    EXPECT_FALSE(snapshot.recoveryPending);
    EXPECT_FALSE(snapshot.terminalManualShutdown);
    EXPECT_EQ(snapshot.cooldownRemainingMs, 0u);
    EXPECT_EQ(snapshot.currentSessionId, 0u);
    EXPECT_EQ(snapshot.lastOpenedSessionId, 0u);
    EXPECT_EQ(snapshot.lastClosedSessionId, 0u);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryEventReportDefaults)
{
    MemRpc::RecoveryEventReport report;
    EXPECT_EQ(report.previousState, MemRpc::ClientLifecycleState::Uninitialized);
    EXPECT_EQ(report.state, MemRpc::ClientLifecycleState::Uninitialized);
    EXPECT_EQ(report.trigger, MemRpc::RecoveryTrigger::Unknown);
    EXPECT_EQ(report.action, MemRpc::RecoveryAction::Ignore);
    EXPECT_FALSE(report.terminalManualShutdown);
    EXPECT_FALSE(report.recoveryPending);
    EXPECT_EQ(report.cooldownDelayMs, 0u);
    EXPECT_EQ(report.cooldownRemainingMs, 0u);
    EXPECT_EQ(report.sessionId, 0u);
    EXPECT_EQ(report.previousSessionId, 0u);
    EXPECT_EQ(report.monotonicMs, 0u);
}

TEST(RpcClientRecoveryPolicyTest, RecoveryEventCallbackCanBeSet)
{
    MemRpc::RpcClient client;
    client.SetRecoveryEventCallback([](const MemRpc::RecoveryEventReport&) {});
}

TEST(RpcClientRecoveryPolicyTest, RecoverySnapshotCanBeQueried)
{
    MemRpc::RpcClient client;
    const MemRpc::RecoveryRuntimeSnapshot snapshot = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Uninitialized);
}
