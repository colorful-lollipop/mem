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
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

}  // namespace

namespace memrpc {

TEST(RpcClientIdleCallbackTest, FiresWhileIdle) {
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho),
                         [](const RpcServerCall&, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  RecoveryPolicy policy;
  policy.idle_timeout_ms = 50;
  policy.idle_notify_interval_ms = 20;
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
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
  CloseHandles(unused_handles);

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho),
                         [](const RpcServerCall& call, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  RecoveryPolicy policy;
  policy.idle_timeout_ms = 50;
  policy.idle_notify_interval_ms = 20;
  policy.onIdle = [&](uint64_t) {
    idle_hits.fetch_add(1);
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  // Send requests periodically to keep the client active.
  for (int i = 0; i < 5; ++i) {
    RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(MiniRpcOpcode::MiniEcho);
    auto future = client.InvokeAsync(call);
    RpcReply reply;
    future.Wait(&reply);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // No idle callbacks should have fired since we kept sending requests.
  EXPECT_EQ(idle_hits.load(), 0);

  client.Shutdown();
  server.Stop();
}

}  // namespace memrpc
