#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/client/typed_future.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/core/codec.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kEchoOpcode = 1u;
constexpr MemRpc::Opcode kBadOpcode = 2u;

// A trivial message type for testing.
struct TestMsg {
  int32_t value = 0;
};

}  // namespace

namespace MemRpc {

template <>
struct CodecTraits<TestMsg> {
  static bool Encode(const TestMsg& msg, std::vector<uint8_t>* bytes) {
    ByteWriter writer;
    if (!writer.WriteInt32(msg.value)) return false;
    return detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const uint8_t* data, std::size_t size, TestMsg* msg) {
    if (msg == nullptr) return false;
    ByteReader reader(data, size);
    return reader.ReadInt32(&msg->value);
  }
};

}  // namespace MemRpc

namespace {

TEST(TypedFutureTest, WaitDecodesReply) {
  auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->ServerHandles());
  server.RegisterHandler(kEchoOpcode,
                         [](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  TestMsg request{42};
  auto future = MemRpc::InvokeTypedAsync<TestMsg, TestMsg>(&client, kEchoOpcode, request);

  TestMsg reply;
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::Ok);
  EXPECT_EQ(reply.value, 42);

  client.Shutdown();
  server.Stop();
}

TEST(TypedFutureTest, ThenDecodesReply) {
  auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->ServerHandles());
  server.RegisterHandler(kEchoOpcode,
                         [](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  TestMsg request{99};
  auto future = MemRpc::InvokeTypedAsync<TestMsg, TestMsg>(&client, kEchoOpcode, request);

  std::atomic<bool> called{false};
  std::mutex mutex;
  TestMsg received;

  future.Then([&](MemRpc::StatusCode status, TestMsg reply) {
    EXPECT_EQ(status, MemRpc::StatusCode::Ok);
    std::lock_guard<std::mutex> lock(mutex);
    received = reply;
    called.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!called.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(called.load());
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received.value, 99);
  }

  client.Shutdown();
  server.Stop();
}

TEST(TypedFutureTest, DecodeFailureReturnsProtocolMismatch) {
  auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->ServerHandles());
  // Return garbage payload that won't decode as TestMsg.
  server.RegisterHandler(kBadOpcode,
                         [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = {0xFF};  // too short for int32
                         });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  // Send an empty call (opcode-only), server returns garbage.
  MemRpc::RpcCall call;
  call.opcode = kBadOpcode;
  MemRpc::TypedFuture<TestMsg> future(client.InvokeAsync(call));

  TestMsg reply;
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::ProtocolMismatch);

  client.Shutdown();
  server.Stop();
}

TEST(TypedFutureTest, ThenSurfacesProtocolMismatch) {
  auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->ServerHandles());
  server.RegisterHandler(kBadOpcode,
                         [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = {0xFF};
                         });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  MemRpc::RpcCall call;
  call.opcode = kBadOpcode;
  MemRpc::TypedFuture<TestMsg> future(client.InvokeAsync(call));

  std::atomic<bool> called{false};
  MemRpc::StatusCode received_status = MemRpc::StatusCode::Ok;

  future.Then([&](MemRpc::StatusCode status, TestMsg) {
    received_status = status;
    called.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!called.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(called.load());
  EXPECT_EQ(received_status, MemRpc::StatusCode::ProtocolMismatch);

  client.Shutdown();
  server.Stop();
}

TEST(TypedFutureTest, IsReadyDelegates) {
  auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->ServerHandles());
  server.RegisterHandler(kEchoOpcode,
                         [](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                           reply->status = MemRpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  TestMsg request{7};
  auto future = MemRpc::InvokeTypedAsync<TestMsg, TestMsg>(&client, kEchoOpcode, request);

  // Wait for completion, then check IsReady.
  TestMsg reply;
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}

}  // namespace
