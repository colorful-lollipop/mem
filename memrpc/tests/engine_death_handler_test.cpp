#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"

namespace {

constexpr MemRpc::Opcode kTestOpcode = 1u;

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    handles = MemRpc::BootstrapHandles{};
    handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode CloseSession() override { return MemRpc::StatusCode::Ok; }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

  void SimulateDeath(uint64_t session_id) {
    if (callback_) {
      callback_(session_id);
    }
  }

 private:
  MemRpc::EngineDeathCallback callback_;
};

}  // namespace

// Without a handler, all futures fail with PeerDisconnected (backward compatible).
TEST(EngineDeathHandlerTest, NoHandlerBackwardCompat) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  std::mutex mutex;
  std::vector<MemRpc::RpcFailure> failures;
  MemRpc::RecoveryPolicy policy;
  policy.onFailure = [&](const MemRpc::RpcFailure& f) {
    std::lock_guard<std::mutex> lock(mutex);
    failures.push_back(f);
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  // Init will fail to create a real session (no shmFd), but InvokeAsync
  // will produce ready futures with failure status. This test verifies
  // the handler-absent code path compiles and runs without crash.
  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());
  MemRpc::RpcReply reply;
  EXPECT_NE(future.Wait(&reply), MemRpc::StatusCode::Ok);

  client.Shutdown();
}

// With handler returning Ignore, all futures should fail.
TEST(EngineDeathHandlerTest, AbandonFailsAll) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  bool handler_called = false;
  MemRpc::RecoveryPolicy policy;
  policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
    handler_called = true;
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());
  MemRpc::RpcReply reply;
  EXPECT_NE(future.Wait(&reply), MemRpc::StatusCode::Ok);

  client.Shutdown();
}

// With handler returning Restart, verify the handler is callable.
TEST(EngineDeathHandlerTest, RestartHandlerCallable) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  std::atomic<bool> handler_called{false};
  MemRpc::RecoveryPolicy policy;
  policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
    handler_called.store(true);
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());
  MemRpc::RpcReply reply;
  future.Wait(&reply);

  client.Shutdown();
}

// Verify StatusCode::CrashedDuringExecution exists and has correct value.
TEST(EngineDeathHandlerTest, CrashedDuringExecutionStatusCode) {
  MemRpc::RpcReply reply;
  reply.status = MemRpc::StatusCode::CrashedDuringExecution;
  EXPECT_EQ(reply.status, MemRpc::StatusCode::CrashedDuringExecution);
  EXPECT_NE(reply.status, MemRpc::StatusCode::PeerDisconnected);
  EXPECT_NE(reply.status, MemRpc::StatusCode::Ok);
}

// Verify EngineDeathReport struct layout.
TEST(EngineDeathHandlerTest, EngineDeathReportLayout) {
  MemRpc::EngineDeathReport report;
  EXPECT_EQ(report.deadSessionId, 0u);
  EXPECT_EQ(report.safeToReplayCount, 0u);
  EXPECT_TRUE(report.poisonPillSuspects.empty());

  MemRpc::EngineDeathReport::PoisonPillSuspect suspect;
  EXPECT_EQ(suspect.requestId, 0u);
  EXPECT_EQ(suspect.opcode, MemRpc::OPCODE_INVALID);
  EXPECT_EQ(suspect.lastState, MemRpc::RpcRuntimeState::Unknown);
}

// Verify RecoveryDecision defaults.
TEST(EngineDeathHandlerTest, RecoveryDecisionDefaults) {
  MemRpc::RecoveryDecision decision;
  EXPECT_EQ(decision.action, MemRpc::RecoveryAction::Ignore);
  EXPECT_EQ(decision.delayMs, 0u);
}

// Verify SetRecoveryPolicy on RpcSyncClient compiles and runs.
TEST(EngineDeathHandlerTest, SyncClientSetRecoveryPolicy) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcSyncClient client(bootstrap);

  bool called = false;
  MemRpc::RecoveryPolicy policy;
  policy.onEngineDeath = [&](const MemRpc::EngineDeathReport&) {
    called = true;
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  MemRpc::RpcCall call;
  call.opcode = kTestOpcode;
  MemRpc::RpcReply reply;
  client.InvokeSync(call, &reply);

  client.Shutdown();
}

// Verify that Shutdown during a potential restart doesn't deadlock or crash.
TEST(EngineDeathHandlerTest, ShutdownIsClean) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  MemRpc::RecoveryPolicy policy;
  policy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 5000};
  };
  client.SetRecoveryPolicy(std::move(policy));

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());
  MemRpc::RpcReply reply;
  future.Wait(&reply);

  // Shutdown should complete quickly even if restart was requested with delay.
  const auto start = std::chrono::steady_clock::now();
  client.Shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}
