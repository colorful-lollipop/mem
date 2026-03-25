#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/server/rpc_server.h"

constexpr MemRpc::Opcode kTestOpcode = 1u;

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    handles = MemRpc::MakeDefaultBootstrapHandles();
    handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
    handles.sessionId = 1;
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode CloseSession() override { return MemRpc::StatusCode::Ok; }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

 private:
  MemRpc::EngineDeathCallback callback_;
};

void CloseHandles(MemRpc::BootstrapHandles& handles) {
  if (handles.shmFd >= 0) close(handles.shmFd);
  if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
  if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
  if (handles.respEventFd >= 0) close(handles.respEventFd);
  if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
  if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
}

}  // namespace

// GTest matcher macros inflate cognitive-complexity counts in this API composition check.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(RpcClientApiTest, PublicHeaderComposes) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);
  client.SetEventCallback([](const MemRpc::RpcEvent& event) {
    EXPECT_GE(event.eventDomain, 0u);
    EXPECT_GE(event.eventType, 0u);
  });

  MemRpc::RpcCall call;
  call.opcode = kTestOpcode;
  call.payload = std::vector<uint8_t>{1, 2, 3};

  MemRpc::RpcReply reply;
  MemRpc::RpcClientRuntimeStats stats;
  EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
  EXPECT_EQ(stats.pendingCalls, 0u);
  EXPECT_EQ(call.priority, MemRpc::Priority::Normal);
  EXPECT_EQ(call.admissionTimeoutMs, 0u);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);

  auto future = client.InvokeAsync(call);
  EXPECT_TRUE(future.IsReady());
  EXPECT_NE(future.Wait(&reply), MemRpc::StatusCode::Ok);
}
// NOLINTEND(readability-function-cognitive-complexity)

TEST(RpcClientApiTest, PublicHeaderSupportsMoveAwareCallAndReplyApis) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  MemRpc::RpcCall call;
  call.opcode = kTestOpcode;
  call.payload = std::vector<uint8_t>{4, 5, 6};

  MemRpc::RpcReply reply;
  auto future = client.InvokeAsync(std::move(call));
  EXPECT_TRUE(future.IsReady());
  EXPECT_NE(future.WaitAndTake(&reply), MemRpc::StatusCode::Ok);
}

TEST(RpcClientApiTest, AdmissionTimeoutCanBeConfiguredIndependently) {
  MemRpc::RpcCall call;
  call.admissionTimeoutMs = 0;
  call.queueTimeoutMs = 250;
  call.execTimeoutMs = 500;

  EXPECT_EQ(call.admissionTimeoutMs, 0u);
  EXPECT_EQ(call.queueTimeoutMs, 250u);
  EXPECT_EQ(call.execTimeoutMs, 500u);
}

TEST(RpcClientApiTest, BootstrapHandlesExposeCreditEventFds) {
  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

  EXPECT_EQ(handles.reqCreditEventFd, -1);
  EXPECT_EQ(handles.respCreditEventFd, -1);
}

TEST(RpcClientApiTest, ThenCallbackInvokedImmediatelyOnAlreadyReadyFuture) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  bool called = false;
  MemRpc::StatusCode received_status = MemRpc::StatusCode::Ok;
  future.Then([&](const MemRpc::RpcReply& reply) {
    called = true;
    received_status = reply.status;
  });

  EXPECT_TRUE(called);
  EXPECT_NE(received_status, MemRpc::StatusCode::Ok);
}

TEST(RpcClientApiTest, ThenWithNullCallbackIsNoOp) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  future.Then(nullptr);  // should not crash
}

TEST(RpcClientApiTest, ThenOnDefaultConstructedFutureIsNoOp) {
  MemRpc::RpcFuture future;
  bool called = false;
  future.Then([&](const MemRpc::RpcReply&) { called = true; });
  EXPECT_FALSE(called);
}

TEST(RpcClientApiTest, ThenUsesExecutorWhenProvided) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  int scheduled = 0;
  bool called = false;
  MemRpc::RpcThenExecutor executor = [&](const std::function<void()>& task) {
    ++scheduled;
    task();
  };

  future.Then([&](const MemRpc::RpcReply&) { called = true; }, executor);

  EXPECT_EQ(scheduled, 1);
  EXPECT_TRUE(called);
}

TEST(RpcClientApiTest, ThenWithoutExecutorRunsInline) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  bool called = false;
  future.Then([&](const MemRpc::RpcReply&) { called = true; });

  EXPECT_TRUE(called);
}

TEST(RpcClientApiTest, SessionReadyCallbackReportsInitialInit) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
  MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
  CloseHandles(unusedHandles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
    reply->status = MemRpc::StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  std::mutex mutex;
  MemRpc::SessionReadyReport captured;
  int calls = 0;
  client.SetSessionReadyCallback([&](const MemRpc::SessionReadyReport& report) {
    std::lock_guard<std::mutex> lock(mutex);
    captured = report;
    ++calls;
  });

  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    bool observed = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      observed = calls == 1;
    }
    if (observed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(calls, 1);
    EXPECT_EQ(captured.reason, MemRpc::SessionOpenReason::InitialInit);
    EXPECT_EQ(captured.previousSessionId, 0u);
    EXPECT_EQ(captured.generation, 1u);
    EXPECT_EQ(captured.scheduledDelayMs, 0u);
    EXPECT_EQ(captured.sessionId, bootstrap->serverHandles().sessionId);
    EXPECT_NE(captured.monotonicMs, 0u);
  }

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientApiTest, FailureCallbackFiresOnAdmissionFailure) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  std::mutex mutex;
  MemRpc::RpcFailure captured{};
  int calls = 0;
  MemRpc::RecoveryPolicy policy;
  policy.onFailure = [&](const MemRpc::RpcFailure& failure) {
    std::lock_guard<std::mutex> lock(mutex);
    captured = failure;
    ++calls;
    return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  MemRpc::RpcCall call;
  call.opcode = kTestOpcode;
  call.priority = MemRpc::Priority::Normal;
  call.admissionTimeoutMs = 1000;
  call.queueTimeoutMs = 0;
  call.execTimeoutMs = 1000;

  auto future = client.InvokeAsync(call);
  MemRpc::RpcReply reply;
  const MemRpc::StatusCode status = future.Wait(&reply);

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(captured.status, status);
  EXPECT_EQ(captured.stage, MemRpc::FailureStage::Admission);
  EXPECT_EQ(captured.opcode, call.opcode);
  EXPECT_EQ(captured.priority, call.priority);
  EXPECT_EQ(captured.admissionTimeoutMs, call.admissionTimeoutMs);
  EXPECT_EQ(captured.queueTimeoutMs, call.queueTimeoutMs);
  EXPECT_EQ(captured.execTimeoutMs, call.execTimeoutMs);
  EXPECT_NE(captured.requestId, 0u);
  EXPECT_EQ(captured.replayHint, MemRpc::ReplayHint::Unknown);
  EXPECT_EQ(captured.lastRuntimeState, MemRpc::RpcRuntimeState::Unknown);
}

TEST(RpcClientApiTest, ShutdownMakesClientPermanentlyTerminal) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  client.Shutdown();

  EXPECT_EQ(client.Init(), MemRpc::StatusCode::ClientClosed);

  MemRpc::RpcReply reply;
  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  EXPECT_TRUE(future.IsReady());
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::ClientClosed);
  EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);

  const MemRpc::RecoveryRuntimeSnapshot snapshot = client.GetRecoveryRuntimeSnapshot();
  EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Closed);
  EXPECT_EQ(snapshot.lastTrigger, MemRpc::RecoveryTrigger::ManualShutdown);
  EXPECT_TRUE(snapshot.terminalManualShutdown);
}

TEST(RpcClientApiTest, ShutdownAfterInitStillRejectsInvokeAsyncImmediately) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
  MemRpc::BootstrapHandles unusedHandles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(unusedHandles), MemRpc::StatusCode::Ok);
  CloseHandles(unusedHandles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestOpcode, [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
    reply->status = MemRpc::StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);

  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);
  client.Shutdown();

  MemRpc::RpcReply reply;
  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  EXPECT_TRUE(future.IsReady());
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::ClientClosed);
  EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);

  server.Stop();
}
