#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
}

bool WaitForCondition(const std::function<bool()>& condition, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return condition();
}

TEST(MiniRpcBackpressureTest, SlotExhaustionAndRecovery) {
  // Use few slots and a slow handler (Sleep) to exhaust capacity.
  MemRpc::DemoBootstrapConfig config;
  config.slot_count = 4;
  config.high_ring_size = 8;
  config.normal_ring_size = 8;
  config.response_ring_size = 8;
  config.max_request_bytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES;
  config.max_response_bytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES;

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>(config);
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.SetOptions({.high_worker_threads = 4, .normal_worker_threads = 4});
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  // Fill all 4 slots with Sleep RPCs that take 500ms each.
  std::vector<std::thread> callers;
  std::atomic<int> completed{0};
  for (int i = 0; i < 4; ++i) {
    callers.emplace_back([&client, &completed]() {
      SleepReply reply;
      MemRpc::StatusCode status = client.Sleep(500, &reply);
      if (status == MemRpc::StatusCode::Ok) {
        completed.fetch_add(1);
      }
    });
  }

  // Give the server time to accept and start processing requests.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // All slots should be in use. A new call should either queue and wait for credit,
  // or succeed after some waiting. We just verify it doesn't crash and completes eventually.
  {
    EchoReply reply;
    MemRpc::StatusCode status = client.Echo("under_pressure", &reply);
    // This call competes for slots with the sleep calls. It will succeed once a slot frees up.
    EXPECT_EQ(status, MemRpc::StatusCode::Ok);
    if (status == MemRpc::StatusCode::Ok) {
      EXPECT_EQ(reply.text, "under_pressure");
    }
  }

  for (auto& t : callers) {
    t.join();
  }

  // After all blocking calls complete, verify normal operation.
  EXPECT_EQ(completed.load(), 4);
  {
    EchoReply reply;
    MemRpc::StatusCode status = client.Echo("recovered", &reply);
    EXPECT_EQ(status, MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.text, "recovered");
  }

  client.Shutdown();
  server.Stop();
}

TEST(MiniRpcBackpressureTest, CreditFlowReleasesBlockedSubmitter) {
  MemRpc::DemoBootstrapConfig config;
  config.slot_count = 2;
  config.high_ring_size = 4;
  config.normal_ring_size = 4;
  config.response_ring_size = 4;
  config.max_request_bytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES;
  config.max_response_bytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES;

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>(config);
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.SetOptions({.high_worker_threads = 2, .normal_worker_threads = 2});
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  // Fill both slots with Sleep RPCs (300ms).
  std::vector<std::thread> callers;
  std::vector<MemRpc::StatusCode> results(2, MemRpc::StatusCode::InvalidArgument);
  for (int i = 0; i < 2; ++i) {
    callers.emplace_back([&client, &results, i]() {
      SleepReply reply;
      results[i] = client.Sleep(300, &reply);
    });
  }

  // Give time for Sleep calls to be dispatched.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Submit a third call — it should block waiting for credit, then succeed
  // once a Sleep completes and frees a slot.
  std::atomic<bool> third_done{false};
  MemRpc::StatusCode third_status = MemRpc::StatusCode::InvalidArgument;
  std::thread third_thread([&]() {
    EchoReply reply;
    third_status = client.Echo("third", &reply);
    third_done.store(true);
  });

  // Give the third call time to block.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Third call may or may not have completed yet (depends on timing).

  for (auto& t : callers) {
    t.join();
  }
  for (const auto& r : results) {
    EXPECT_EQ(r, MemRpc::StatusCode::Ok);
  }

  // The third call should complete after credit is signaled.
  ASSERT_TRUE(WaitForCondition([&third_done] { return third_done.load(); }, 2000))
      << "third call should unblock after credit";
  third_thread.join();
  EXPECT_EQ(third_status, MemRpc::StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
