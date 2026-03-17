#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kEchoOpcode = static_cast<MemRpc::Opcode>(202);

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) {
    close(h.shmFd);
  }
  if (h.highReqEventFd >= 0) {
    close(h.highReqEventFd);
  }
  if (h.normalReqEventFd >= 0) {
    close(h.normalReqEventFd);
  }
  if (h.respEventFd >= 0) {
    close(h.respEventFd);
  }
  if (h.reqCreditEventFd >= 0) {
    close(h.reqCreditEventFd);
  }
  if (h.respCreditEventFd >= 0) {
    close(h.respCreditEventFd);
  }
}

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

class CountingBootstrapChannel final : public MemRpc::IBootstrapChannel {
 public:
  explicit CountingBootstrapChannel(std::shared_ptr<MemRpc::DevBootstrapChannel> delegate)
      : delegate_(std::move(delegate)) {}

  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    openCount_.fetch_add(1);
    return delegate_->OpenSession(handles);
  }

  MemRpc::StatusCode CloseSession() override {
    closeCount_.fetch_add(1);
    return delegate_->CloseSession();
  }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    deathCallback_ = std::move(callback);
    delegate_->SetEngineDeathCallback(deathCallback_);
  }

  [[nodiscard]] int openCount() const {
    return openCount_.load();
  }

  [[nodiscard]] int closeCount() const {
    return closeCount_.load();
  }

 private:
  std::shared_ptr<MemRpc::DevBootstrapChannel> delegate_;
  MemRpc::EngineDeathCallback deathCallback_;
  std::atomic<int> openCount_{0};
  std::atomic<int> closeCount_{0};
};

}  // namespace

namespace MemRpc {

TEST(RpcClientExternalRecoveryTest, RequestExternalRecoveryReusesRestartFlow) {
  auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  RpcServer server;
  server.SetBootstrapHandles(rawBootstrap->serverHandles());
  server.RegisterHandler(kEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
  RpcClient client(bootstrap);
  std::mutex sessionMutex;
  std::vector<SessionReadyReport> reports;
  client.SetSessionReadyCallback([&](const SessionReadyReport& report) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    reports.push_back(report);
  });

  std::atomic<int> engineDeathCalls{0};
  RecoveryPolicy policy;
  policy.onEngineDeath = [&](const EngineDeathReport&) {
    engineDeathCalls.fetch_add(1);
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  client.RequestExternalRecovery(
      {ExternalRecoverySignal::ChannelHealthUnhealthy, rawBootstrap->serverHandles().sessionId, 0});

  ASSERT_TRUE(WaitFor([&]() { return bootstrap->closeCount() >= 1; },
                      std::chrono::milliseconds(500)));
  ASSERT_TRUE(WaitFor([&]() { return bootstrap->openCount() >= 2; },
                      std::chrono::milliseconds(500)));
  ASSERT_TRUE(WaitFor([&]() {
    std::lock_guard<std::mutex> lock(sessionMutex);
    return reports.size() >= 2;
  }, std::chrono::milliseconds(500)));
  EXPECT_EQ(engineDeathCalls.load(), 0);

  SessionReadyReport initialReport;
  SessionReadyReport recoveredReport;
  {
    std::lock_guard<std::mutex> lock(sessionMutex);
    ASSERT_GE(reports.size(), 2u);
    initialReport = reports[0];
    recoveredReport = reports[1];
  }
  EXPECT_EQ(initialReport.reason, SessionOpenReason::InitialInit);
  EXPECT_EQ(initialReport.previousSessionId, 0u);
  EXPECT_EQ(initialReport.generation, 1u);
  EXPECT_EQ(recoveredReport.reason, SessionOpenReason::ExternalRecovery);
  EXPECT_EQ(recoveredReport.previousSessionId, initialReport.sessionId);
  EXPECT_EQ(recoveredReport.generation, 2u);
  EXPECT_EQ(recoveredReport.scheduledDelayMs, 0u);
  EXPECT_NE(recoveredReport.sessionId, 0u);

  RpcCall call;
  call.opcode = kEchoOpcode;
  auto future = client.InvokeAsync(call);
  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientExternalRecoveryTest, RequestExternalRecoveryHonorsCooldownGate) {
  auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  RpcServer server;
  server.SetBootstrapHandles(rawBootstrap->serverHandles());
  server.RegisterHandler(kEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  const uint64_t sessionId = rawBootstrap->serverHandles().sessionId;
  client.RequestExternalRecovery(
      {ExternalRecoverySignal::ChannelHealthTimeout, sessionId, 200});

  RpcCall call;
  call.opcode = kEchoOpcode;
  auto blockedFuture = client.InvokeAsync(call);
  RpcReply blockedReply;
  EXPECT_EQ(blockedFuture.Wait(&blockedReply), StatusCode::CooldownActive);

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  auto recoveredFuture = client.InvokeAsync(call);
  RpcReply recoveredReply;
  EXPECT_EQ(recoveredFuture.Wait(&recoveredReply), StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientExternalRecoveryTest, RuntimeStatsExposeCooldownRemaining) {
  auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  RpcServer server;
  server.SetBootstrapHandles(rawBootstrap->serverHandles());
  server.RegisterHandler(kEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(rawBootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  client.RequestExternalRecovery(
      {ExternalRecoverySignal::ChannelHealthTimeout, rawBootstrap->serverHandles().sessionId, 200});

  const RpcClientRuntimeStats stats = client.GetRuntimeStats();
  EXPECT_GT(stats.cooldownRemainingMs, 0u);
  EXPECT_LE(stats.cooldownRemainingMs, 200u);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientExternalRecoveryTest, ShutdownDisablesLaterRecoverySignals) {
  auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  RpcServer server;
  server.SetBootstrapHandles(rawBootstrap->serverHandles());
  server.RegisterHandler(kEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  client.Shutdown();
  client.RequestExternalRecovery(
      {ExternalRecoverySignal::ChannelHealthTimeout, rawBootstrap->serverHandles().sessionId, 200});

  const RpcClientRuntimeStats stats = client.GetRuntimeStats();
  EXPECT_FALSE(stats.recoveryPending);
  EXPECT_EQ(stats.cooldownRemainingMs, 0u);
  EXPECT_EQ(bootstrap->openCount(), 1);

  RpcCall call;
  call.opcode = kEchoOpcode;
  auto future = client.InvokeAsync(call);
  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::ClientClosed);
  EXPECT_EQ(bootstrap->openCount(), 1);

  server.Stop();
}

TEST(RpcClientExternalRecoveryTest, RequestExternalRecoveryCanWaitInternallyAcrossCooldown) {
  auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(rawBootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  RpcServer server;
  server.SetBootstrapHandles(rawBootstrap->serverHandles());
  server.RegisterHandler(kEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  const uint64_t sessionId = rawBootstrap->serverHandles().sessionId;
  client.RequestExternalRecovery(
      {ExternalRecoverySignal::ChannelHealthTimeout, sessionId, 200});

  RpcCall call;
  call.opcode = kEchoOpcode;
  call.waitForRecovery = true;
  call.recoveryTimeoutMs = 600;

  const auto start = std::chrono::steady_clock::now();
  auto future = client.InvokeAsync(call);
  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::Ok);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_GE(elapsed.count(), 180);
  EXPECT_LT(elapsed.count(), 1000);

  client.Shutdown();
  server.Stop();
}

}  // namespace MemRpc
