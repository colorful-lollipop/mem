#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/common/minirpc_codec.h"
#include "apps/minirpc/parent/minirpc_async_client.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/server/rpc_server.h"

#include "apps/minirpc/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shm_fd >= 0) close(h.shm_fd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

void RunMiniRpcServerProcess(MemRpc::BootstrapHandles handles) {
  MemRpc::RpcServer server;
  server.SetBootstrapHandles(handles);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  if (server.Start() != MemRpc::StatusCode::Ok) {
    _exit(2);
  }
  server.Run();
  _exit(0);
}

TEST(MiniRpcClientTest, SyncAndAsyncCallsRoundTrip) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcAsyncClient async_client(bootstrap);
  ASSERT_EQ(async_client.Init(), MemRpc::StatusCode::Ok);

  EchoRequest echo_request;
  echo_request.text = "ping";
  auto future = async_client.EchoAsync(echo_request);
  MemRpc::RpcReply echo_rpc_reply;
  ASSERT_EQ(future.WaitAndTake(&echo_rpc_reply), MemRpc::StatusCode::Ok);
  EchoReply echo_reply;
  ASSERT_TRUE(DecodeMessage<EchoReply>(echo_rpc_reply.payload, &echo_reply));
  EXPECT_EQ(echo_reply.text, "ping");

  async_client.Shutdown();

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  AddReply add_reply;
  EXPECT_EQ(client.Add(4, 5, &add_reply), MemRpc::StatusCode::Ok);
  EXPECT_EQ(add_reply.sum, 9);

  client.Shutdown();
  server.Stop();
}

TEST(MiniRpcClientTest, HighPrioritySleepCompletesBeforeNormalBacklog) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1});
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcAsyncClient async_client(bootstrap);
  ASSERT_EQ(async_client.Init(), MemRpc::StatusCode::Ok);

  SleepRequest slow_request;
  slow_request.delay_ms = 200;
  auto slow_future = async_client.SleepAsync(slow_request, MemRpc::Priority::Normal, 1000);

  SleepRequest fast_request;
  fast_request.delay_ms = 5;
  MemRpc::RpcReply fast_reply;
  EXPECT_EQ(async_client.SleepAsync(fast_request, MemRpc::Priority::High, 1000).Wait(&fast_reply),
            MemRpc::StatusCode::Ok);

  MemRpc::RpcReply slow_reply;
  EXPECT_EQ(slow_future.Wait(&slow_reply), MemRpc::StatusCode::Ok);

  async_client.Shutdown();
  server.Stop();
}

TEST(MiniRpcClientTest, ProcessExitDuringHandlingFailsPendingAndRecoversAfterRestart) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  const pid_t first_child = fork();
  ASSERT_GE(first_child, 0);
  if (first_child == 0) {
    RunMiniRpcServerProcess(bootstrap->server_handles());
  }

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  SleepRequest sleep_request;
  sleep_request.delay_ms = 1000;
  std::vector<uint8_t> sleep_payload;
  ASSERT_TRUE(EncodeMessage<SleepRequest>(sleep_request, &sleep_payload));

  MemRpc::RpcCall sleep_call;
  sleep_call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniSleep);
  sleep_call.exec_timeout_ms = 5000;
  sleep_call.payload = sleep_payload;
  auto sleep_future = client.InvokeAsync(sleep_call);

  MemRpc::RpcCall crash_call;
  crash_call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest);
  crash_call.priority = MemRpc::Priority::High;
  auto crash_future = client.InvokeAsync(crash_call);

  int first_status = 0;
  ASSERT_EQ(waitpid(first_child, &first_status, 0), first_child);
  ASSERT_TRUE(WIFEXITED(first_status));
  EXPECT_EQ(WEXITSTATUS(first_status), 99);

  bootstrap->SimulateEngineDeathForTest();

  MemRpc::RpcReply sleep_reply;
  EXPECT_EQ(sleep_future.Wait(&sleep_reply), MemRpc::StatusCode::PeerDisconnected);
  MemRpc::RpcReply crash_reply;
  EXPECT_EQ(crash_future.Wait(&crash_reply), MemRpc::StatusCode::PeerDisconnected);

  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);
  const pid_t second_child = fork();
  ASSERT_GE(second_child, 0);
  if (second_child == 0) {
    RunMiniRpcServerProcess(bootstrap->server_handles());
  }

  EchoRequest echo_request;
  echo_request.text = "after-restart";
  std::vector<uint8_t> echo_payload;
  ASSERT_TRUE(EncodeMessage<EchoRequest>(echo_request, &echo_payload));

  MemRpc::RpcCall echo_call;
  echo_call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniEcho);
  echo_call.payload = echo_payload;

  MemRpc::RpcReply echo_rpc_reply;
  ASSERT_EQ(client.InvokeAsync(echo_call).WaitAndTake(&echo_rpc_reply), MemRpc::StatusCode::Ok);
  EchoReply echo_reply;
  ASSERT_TRUE(DecodeMessage<EchoReply>(echo_rpc_reply.payload, &echo_reply));
  EXPECT_EQ(echo_reply.text, "after-restart");

  client.Shutdown();
  kill(second_child, SIGTERM);
  waitpid(second_child, nullptr, 0);
}

TEST(MiniRpcClientTest, TypedThenDecodesReply) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  EchoRequest req;
  req.text = "typed-then";

  std::atomic<bool> called{false};
  std::mutex mutex;
  EchoReply received;

  auto future = MemRpc::InvokeTyped(&client, static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniEcho), req);
  MemRpc::Then<EchoReply>(std::move(future),
      [&](MemRpc::StatusCode status, EchoReply reply) {
        EXPECT_EQ(status, MemRpc::StatusCode::Ok);
        std::lock_guard<std::mutex> lock(mutex);
        received = std::move(reply);
        called.store(true);
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!called.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(called.load());
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received.text, "typed-then");
  }

  client.Shutdown();
  server.Stop();
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
