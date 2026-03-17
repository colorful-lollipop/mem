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

constexpr MemRpc::Opcode kTestEchoOpcode = static_cast<MemRpc::Opcode>(200);

class CountingBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  explicit CountingBootstrapChannel(std::shared_ptr<MemRpc::DevBootstrapChannel> delegate)
      : delegate_(std::move(delegate)) {}

  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    ++openCount_;
    return delegate_->OpenSession(handles);
  }

  MemRpc::StatusCode CloseSession() override {
    ++closeCount_;
    return delegate_->CloseSession();
  }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    delegate_->SetEngineDeathCallback(std::move(callback));
  }

  [[nodiscard]] int openCount() const { return openCount_.load(); }
  [[nodiscard]] int closeCount() const { return closeCount_.load(); }

 private:
  std::shared_ptr<MemRpc::DevBootstrapChannel> delegate_;
  std::atomic<int> openCount_{0};
  std::atomic<int> closeCount_{0};
};

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

}  // namespace

namespace MemRpc {

TEST(RpcClientIdleCallbackTest, FiresWhileIdle) {
  auto bootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  RecoveryPolicy policy;
  policy.onIdle = [&](uint64_t idle_ms) {
    if (idle_ms > 0) {
      idle_hits.fetch_add(1);
    }
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  ASSERT_EQ(client.Init(), StatusCode::Ok);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline && idle_hits.load() < 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(idle_hits.load(), 2);
  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIdleCallbackTest, ActivityResetsIdleTimer) {
  auto bootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall& call, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> long_idle_hits{0};
  RecoveryPolicy policy;
  policy.onIdle = [&](uint64_t idle_ms) {
    if (idle_ms >= 120) {
      long_idle_hits.fetch_add(1);
    }
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  // Keep resetting idle time before it can grow into a longer idle window.
  for (int i = 0; i < 5; ++i) {
    RpcCall call;
    call.opcode = kTestEchoOpcode;
    auto future = client.InvokeAsync(call);
    RpcReply reply;
    future.Wait(&reply);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_EQ(long_idle_hits.load(), 0);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIdleCallbackTest, CloseSessionPolicyReopensOnDemand) {
  auto raw_bootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(raw_bootstrap->OpenSession(unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(raw_bootstrap->serverHandles());
  server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  auto bootstrap = std::make_shared<CountingBootstrapChannel>(raw_bootstrap);
  RpcClient client(bootstrap);
  std::mutex sessionMutex;
  std::vector<SessionReadyReport> reports;
  std::mutex recoveryEventMutex;
  std::vector<RecoveryEventReport> recoveryEvents;
  client.SetSessionReadyCallback([&](const SessionReadyReport& report) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    reports.push_back(report);
  });
  client.SetRecoveryEventCallback([&](const RecoveryEventReport& report) {
    std::lock_guard<std::mutex> lock(recoveryEventMutex);
    recoveryEvents.push_back(report);
  });
  RecoveryPolicy policy;
  policy.onIdle = [](uint64_t) {
    return RecoveryDecision{RecoveryAction::CloseSession, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < close_deadline && bootstrap->closeCount() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(bootstrap->closeCount(), 1);
  const auto idleClosedSnapshot = client.GetRecoveryRuntimeSnapshot();
  EXPECT_EQ(idleClosedSnapshot.lifecycleState, ClientLifecycleState::IdleClosed);
  EXPECT_EQ(idleClosedSnapshot.lastTrigger, RecoveryTrigger::IdlePolicy);
  EXPECT_FALSE(idleClosedSnapshot.terminalManualShutdown);

  RpcCall call;
  call.opcode = kTestEchoOpcode;
  auto future = client.InvokeAsync(call);
  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::Ok);
  EXPECT_GE(bootstrap->openCount(), 2);
  const auto reopenedSnapshot = client.GetRecoveryRuntimeSnapshot();
  EXPECT_EQ(reopenedSnapshot.lifecycleState, ClientLifecycleState::Active);
  EXPECT_EQ(reopenedSnapshot.lastTrigger, RecoveryTrigger::DemandReconnect);

  const auto reopen_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < reopen_deadline) {
    bool observed = false;
    {
      std::lock_guard<std::mutex> lock(sessionMutex);
      observed = reports.size() >= 2;
    }
    {
      std::lock_guard<std::mutex> lock(recoveryEventMutex);
      observed = observed && recoveryEvents.size() >= 3;
    }
    if (observed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  SessionReadyReport initialReport;
  SessionReadyReport reopenedReport;
  {
    std::lock_guard<std::mutex> lock(sessionMutex);
    ASSERT_GE(reports.size(), 2u);
    initialReport = reports[0];
    reopenedReport = reports[1];
  }
  EXPECT_EQ(initialReport.reason, SessionOpenReason::InitialInit);
  EXPECT_EQ(reopenedReport.reason, SessionOpenReason::DemandReconnect);
  EXPECT_EQ(reopenedReport.previousSessionId, initialReport.sessionId);
  EXPECT_EQ(reopenedReport.generation, 2u);
  EXPECT_EQ(reopenedReport.scheduledDelayMs, 0u);

  RecoveryEventReport idleClosedEvent;
  RecoveryEventReport recoveringEvent;
  RecoveryEventReport reopenedEvent;
  {
    std::lock_guard<std::mutex> lock(recoveryEventMutex);
    ASSERT_GE(recoveryEvents.size(), 3u);
    idleClosedEvent = recoveryEvents[1];
    recoveringEvent = recoveryEvents[2];
    reopenedEvent = recoveryEvents.back();
  }
  EXPECT_EQ(idleClosedEvent.state, ClientLifecycleState::IdleClosed);
  EXPECT_EQ(idleClosedEvent.trigger, RecoveryTrigger::IdlePolicy);
  EXPECT_EQ(recoveringEvent.state, ClientLifecycleState::Recovering);
  EXPECT_EQ(recoveringEvent.trigger, RecoveryTrigger::DemandReconnect);
  EXPECT_EQ(reopenedEvent.state, ClientLifecycleState::Active);
  EXPECT_EQ(reopenedEvent.trigger, RecoveryTrigger::DemandReconnect);

  client.Shutdown();
  server.Stop();
}

}  // namespace MemRpc
