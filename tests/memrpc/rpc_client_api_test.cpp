#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

constexpr memrpc::Opcode kTestOpcode = 1u;

namespace {

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

 private:
  MemRpc::EngineDeathCallback callback_;
};

}  // namespace

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
  MemRpc::BootstrapHandles handles;

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
  future.Then([&](MemRpc::RpcReply reply) {
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
  future.Then([&](MemRpc::RpcReply) { called = true; });
  EXPECT_FALSE(called);
}

TEST(RpcClientApiTest, ThenUsesExecutorWhenProvided) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  int scheduled = 0;
  bool called = false;
  MemRpc::RpcThenExecutor executor = [&](std::function<void()> task) {
    ++scheduled;
    task();
  };

  future.Then([&](MemRpc::RpcReply) { called = true; }, executor);

  EXPECT_EQ(scheduled, 1);
  EXPECT_TRUE(called);
}

TEST(RpcClientApiTest, ThenWithoutExecutorRunsInline) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  bool called = false;
  future.Then([&](MemRpc::RpcReply) { called = true; });

  EXPECT_TRUE(called);
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
  EXPECT_EQ(captured.flags, call.flags);
  EXPECT_EQ(captured.admissionTimeoutMs, call.admissionTimeoutMs);
  EXPECT_EQ(captured.queueTimeoutMs, call.queueTimeoutMs);
  EXPECT_EQ(captured.execTimeoutMs, call.execTimeoutMs);
  EXPECT_NE(captured.requestId, 0u);
  EXPECT_EQ(captured.replayHint, MemRpc::ReplayHint::Unknown);
  EXPECT_EQ(captured.lastRuntimeState, MemRpc::RpcRuntimeState::Unknown);
}
