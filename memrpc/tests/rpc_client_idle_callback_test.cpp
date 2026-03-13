#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr memrpc::Opcode kTestEchoOpcode = static_cast<memrpc::Opcode>(200);

void CloseHandles(memrpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
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
  server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall&, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  RecoveryPolicy policy;
  policy.idleTimeoutMs = 50;
  policy.idleNotifyIntervalMs = 20;
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
  server.RegisterHandler(kTestEchoOpcode, [](const RpcServerCall& call, RpcServerReply* reply) {
                           reply->status = StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  RecoveryPolicy policy;
  policy.idleTimeoutMs = 50;
  policy.idleNotifyIntervalMs = 20;
  policy.onIdle = [&](uint64_t) {
    idle_hits.fetch_add(1);
    return RecoveryDecision{RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  // Send requests periodically to keep the client active.
  for (int i = 0; i < 5; ++i) {
    RpcCall call;
    call.opcode = kTestEchoOpcode;
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
