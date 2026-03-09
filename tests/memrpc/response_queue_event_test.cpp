#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace {

bool WaitForValue(const std::atomic<int>& value, int expected, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (value.load() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return value.load() == expected;
}

}  // namespace

TEST(ResponseQueueEventTest, EventDoesNotRequirePendingRequest) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  std::vector<uint8_t> received_payload;
  client.SetEventCallback([&](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 10u);
    EXPECT_EQ(event.event_type, 20u);
    received_payload = event.payload;
    event_count.fetch_add(1);
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcEvent event;
  event.event_domain = 10;
  event.event_type = 20;
  event.payload = {7, 8, 9};
  ASSERT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);

  ASSERT_TRUE(WaitForValue(event_count, 1, 200));
  EXPECT_EQ(received_payload, event.payload);

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, ReplyAndEventCanShareOneResponseQueue) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&server](const memrpc::RpcServerCall& call,
                                   memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           memrpc::RpcEvent event;
                           event.event_domain = 1;
                           event.event_type = 2;
                           event.flags = 3;
                           event.payload = {4, 5, 6};
                           EXPECT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->engine_code = 12;
                           reply->detail_code = 34;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  client.SetEventCallback([&](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 1u);
    EXPECT_EQ(event.event_type, 2u);
    EXPECT_EQ(event.flags, 3u);
    EXPECT_EQ(event.payload, std::vector<uint8_t>({4, 5, 6}));
    event_count.fetch_add(1);
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = {9, 10, 11};

  memrpc::RpcReply reply;
  ASSERT_EQ(client.InvokeSync(call, &reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(reply.payload, call.payload);
  EXPECT_EQ(reply.engine_code, 12);
  EXPECT_EQ(reply.detail_code, 34);
  ASSERT_TRUE(WaitForValue(event_count, 1, 200));

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, EventRespectsConfiguredResponseLimit) {
  memrpc::DemoBootstrapConfig config;
  config.max_response_bytes = 64;
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcEvent event;
  event.event_domain = 3;
  event.event_type = 4;
  event.payload.assign(65, 0x2a);
  EXPECT_EQ(server.PublishEvent(event), memrpc::StatusCode::InvalidArgument);

  client.Shutdown();
  server.Stop();
}
