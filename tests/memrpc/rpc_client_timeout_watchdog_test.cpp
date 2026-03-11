#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

#include "apps/minirpc/protocol.h"

using OHOS::Security::VirusProtectionService::MiniRpc::MiniRpcOpcode;

namespace {

void CloseHandles(memrpc::BootstrapHandles& h) {
  if (h.shm_fd >= 0) close(h.shm_fd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

}  // namespace

namespace memrpc {

TEST(RpcClientTimeoutWatchdogTest, TriggersExecTimeoutForSlowHandler) {
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho),
                         [](const RpcServerCall&, RpcServerReply* reply) {
                           // Sleep long enough that exec timeout fires.
                           std::this_thread::sleep_for(std::chrono::milliseconds(500));
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  std::atomic<bool> got_failure{false};
  FailureStage captured_stage = FailureStage::Admission;
  StatusCode captured_status = StatusCode::Ok;
  ReplayHint captured_hint = ReplayHint::Unknown;
  RecoveryPolicy policy;
  policy.onFailure = [&](const RpcFailure& failure) {
    got_failure.store(true);
    captured_stage = failure.stage;
    captured_status = failure.status;
    captured_hint = failure.replay_hint;
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));

  RpcCall call;
  call.opcode = static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho);
  call.queue_timeout_ms = 5000;
  call.exec_timeout_ms = 100;  // Short exec timeout.
  auto future = client.InvokeAsync(call);

  RpcReply reply;
  const StatusCode wait_status = future.Wait(&reply);
  EXPECT_EQ(wait_status, StatusCode::ExecTimeout);
  EXPECT_TRUE(got_failure.load());
  EXPECT_EQ(captured_stage, FailureStage::Timeout);
  EXPECT_EQ(captured_status, StatusCode::ExecTimeout);
  EXPECT_EQ(captured_hint, ReplayHint::MaybeExecuted);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientTimeoutWatchdogTest, TriggersQueueTimeoutWhenStuckInQueue) {
  // Create a server that does NOT start, so requests stay in Queued state.
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  // Start server but register a handler that blocks forever to keep
  // the first request occupying the only worker thread, so subsequent
  // requests stay queued.
  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  std::atomic<bool> server_started{false};
  server.RegisterHandler(static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho),
                         [&](const RpcServerCall&, RpcServerReply* reply) {
                           server_started.store(true);
                           std::this_thread::sleep_for(std::chrono::seconds(5));
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  // First request: blocks the server worker.
  RpcCall blocker;
  blocker.opcode = static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho);
  blocker.queue_timeout_ms = 10000;
  blocker.exec_timeout_ms = 10000;
  auto blocker_future = client.InvokeAsync(blocker);

  // Wait until server picks up the blocker.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!server_started.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(server_started.load());

  // Second request: will sit in queue since the worker is busy.
  std::atomic<bool> got_failure{false};
  StatusCode captured_status = StatusCode::Ok;
  ReplayHint captured_hint = ReplayHint::Unknown;
  RecoveryPolicy policy2;
  policy2.onFailure = [&](const RpcFailure& failure) {
    if (failure.stage == FailureStage::Timeout) {
      got_failure.store(true);
      captured_status = failure.status;
      captured_hint = failure.replay_hint;
    }
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy2));

  RpcCall queued_call;
  queued_call.opcode = static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho);
  queued_call.queue_timeout_ms = 100;  // Short queue timeout.
  queued_call.exec_timeout_ms = 5000;
  auto queued_future = client.InvokeAsync(queued_call);

  RpcReply reply;
  const StatusCode wait_status = queued_future.Wait(&reply);
  EXPECT_EQ(wait_status, StatusCode::QueueTimeout);
  EXPECT_TRUE(got_failure.load());
  EXPECT_EQ(captured_status, StatusCode::QueueTimeout);
  EXPECT_EQ(captured_hint, ReplayHint::SafeToReplay);

  client.Shutdown();
  server.Stop();
}

}  // namespace memrpc
