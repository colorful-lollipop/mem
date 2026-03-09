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

TEST(RpcClientIntegrationTest, InvokeAsyncAndInvokeSyncRoundTrip) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->engine_code = 7;
                           reply->detail_code = 9;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  client.SetEventCallback([](const memrpc::RpcEvent& event) {
    EXPECT_GE(event.event_domain, 0u);
    EXPECT_GE(event.event_type, 0u);
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{4, 5, 6};

  auto future = client.InvokeAsync(call);
  memrpc::RpcReply async_reply;
  EXPECT_EQ(future.Wait(&async_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(async_reply.payload, call.payload);
  EXPECT_EQ(async_reply.engine_code, 7);
  EXPECT_EQ(async_reply.detail_code, 9);

  memrpc::RpcReply sync_reply;
  EXPECT_EQ(client.InvokeSync(call, &sync_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(sync_reply.payload, call.payload);
  EXPECT_EQ(sync_reply.engine_code, 7);
  EXPECT_EQ(sync_reply.detail_code, 9);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, PendingRequestFailsPromptlyAfterEngineDeath) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           std::this_thread::sleep_for(std::chrono::milliseconds(200));
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.exec_timeout_ms = 5000;
  call.payload = std::vector<uint8_t>{1, 2, 3};

  auto future = client.InvokeAsync(call);
  bootstrap->SimulateEngineDeathForTest();

  const auto start = std::chrono::steady_clock::now();
  memrpc::RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), memrpc::StatusCode::PeerDisconnected);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_LT(elapsed.count(), 1000);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, InvokeSyncReconnectsAfterEngineRestart) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer first_server;
  first_server.SetBootstrapHandles(bootstrap->server_handles());
  first_server.RegisterHandler(memrpc::Opcode::ScanFile,
                               [](const memrpc::RpcServerCall& call,
                                  memrpc::RpcServerReply* reply) {
                                 ASSERT_NE(reply, nullptr);
                                 reply->status = memrpc::StatusCode::Ok;
                                 reply->engine_code = 11;
                                 reply->payload = call.payload;
                               });
  ASSERT_EQ(first_server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(bootstrap->Connect(&first_handles), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{9, 8, 7};

  memrpc::RpcReply first_reply;
  ASSERT_EQ(client.InvokeSync(call, &first_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(first_reply.engine_code, 11);

  bootstrap->SimulateEngineDeathForTest();
  first_server.Stop();

  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(bootstrap->Connect(&second_handles), memrpc::StatusCode::Ok);
  ASSERT_NE(first_handles.session_id, second_handles.session_id);

  memrpc::RpcServer second_server;
  second_server.SetBootstrapHandles(bootstrap->server_handles());
  second_server.RegisterHandler(memrpc::Opcode::ScanFile,
                                [](const memrpc::RpcServerCall& call,
                                   memrpc::RpcServerReply* reply) {
                                  ASSERT_NE(reply, nullptr);
                                  reply->status = memrpc::StatusCode::Ok;
                                  reply->engine_code = 22;
                                  reply->payload = call.payload;
                                });
  ASSERT_EQ(second_server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcReply second_reply;
  EXPECT_EQ(client.InvokeSync(call, &second_reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(second_reply.engine_code, 22);
  EXPECT_EQ(second_reply.payload, call.payload);

  close(first_handles.shm_fd);
  close(first_handles.high_req_event_fd);
  close(first_handles.normal_req_event_fd);
  close(first_handles.resp_event_fd);
  close(second_handles.shm_fd);
  close(second_handles.high_req_event_fd);
  close(second_handles.normal_req_event_fd);
  close(second_handles.resp_event_fd);

  client.Shutdown();
  second_server.Stop();
}

TEST(RpcClientIntegrationTest, InvokeAsyncRejectsPayloadLargerThanConfiguredRequestLimit) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 4,
          .normal_ring_size = 4,
          .response_ring_size = 4,
          .slot_count = 2,
          .max_request_bytes = 8,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>(9, 0x7f);

  memrpc::RpcReply reply;
  EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), memrpc::StatusCode::InvalidArgument);

  client.Shutdown();
  server.Stop();
}
