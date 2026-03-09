#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "core/session.h"
#include "core/session_test_hook.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace {

bool WaitForAtomicAtLeast(const std::atomic<int>& value, int expected, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (value.load() >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return value.load() >= expected;
}

class RingTraceRecorder {
 public:
  RingTraceRecorder() {
    active_ = this;
    memrpc::SetRingTraceCallbackForTest(&RingTraceRecorder::Record);
  }

  ~RingTraceRecorder() {
    memrpc::ClearRingTraceCallbackForTest();
    active_ = nullptr;
  }

  size_t UniqueThreadCount(memrpc::RingTraceOperation operation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_[static_cast<size_t>(operation)].size();
  }

 private:
  static void Record(memrpc::RingTraceOperation operation, uint64_t thread_token) {
    if (active_ != nullptr) {
      active_->RecordImpl(operation, thread_token);
    }
  }

  void RecordImpl(memrpc::RingTraceOperation operation, uint64_t thread_token) {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_[static_cast<size_t>(operation)].insert(thread_token);
  }

  static inline RingTraceRecorder* active_ = nullptr;
  mutable std::mutex mutex_;
  std::set<uint64_t> threads_[static_cast<size_t>(memrpc::RingTraceOperation::Count)]{};
};

}  // namespace

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

TEST(RpcClientIntegrationTest, ConcurrentInvokeAsyncKeepsRepliesMatchedToRequests) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 2, .normal_worker_threads = 2});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  std::vector<std::thread> threads;
  std::mutex results_mutex;
  std::vector<int> mismatches;
  constexpr int kRequestCount = 32;

  for (int i = 0; i < kRequestCount; ++i) {
    threads.emplace_back([&client, &results_mutex, &mismatches, i] {
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.payload = {static_cast<uint8_t>(i), static_cast<uint8_t>(i + 1)};

      memrpc::RpcReply reply;
      const memrpc::StatusCode status = client.InvokeAsync(call).Wait(&reply);
      std::lock_guard<std::mutex> lock(results_mutex);
      if (status != memrpc::StatusCode::Ok || reply.payload != call.payload) {
        mismatches.push_back(i);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(mismatches.empty());

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, SharedRequestRingsUseSingleProducerAndConsumerThreads) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 2, .normal_worker_threads = 2});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  RingTraceRecorder recorder;
  std::vector<std::thread> threads;
  constexpr int kCallsPerPriority = 8;
  for (int i = 0; i < kCallsPerPriority; ++i) {
    threads.emplace_back([&client, i] {
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.priority = memrpc::Priority::High;
      call.payload = {static_cast<uint8_t>(i), 0xa1};
      memrpc::RpcReply reply;
      EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), memrpc::StatusCode::Ok);
      EXPECT_EQ(reply.payload, call.payload);
    });
    threads.emplace_back([&client, i] {
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.priority = memrpc::Priority::Normal;
      call.payload = {static_cast<uint8_t>(i), 0xb2};
      memrpc::RpcReply reply;
      EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), memrpc::StatusCode::Ok);
      EXPECT_EQ(reply.payload, call.payload);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PushHighRequest), 1u);
  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PushNormalRequest), 1u);
  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PopHighRequest), 1u);
  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PopNormalRequest), 1u);

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

TEST(RpcClientIntegrationTest, InvokeSyncWithoutExplicitTimeoutCanWaitPastOneSecond) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.queue_timeout_ms = 0;
  call.exec_timeout_ms = 0;
  call.payload = std::vector<uint8_t>{0x41, 0x42};

  const auto start = std::chrono::steady_clock::now();
  memrpc::RpcReply reply;
  EXPECT_EQ(client.InvokeSync(call, &reply), memrpc::StatusCode::Ok);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_GE(elapsed.count(), 1100);
  EXPECT_EQ(reply.payload, call.payload);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, InvokeSyncWaitsForAdmissionTimeoutInsteadOfReturningPeerDisconnected) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 1,
          .normal_ring_size = 1,
          .response_ring_size = 2,
          .slot_count = 1,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  std::atomic<int> started{0};
  std::atomic<bool> release{false};

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&started, &release](const memrpc::RpcServerCall& call,
                                              memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           started.fetch_add(1);
                           while (!release.load()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           }
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall first_call;
  first_call.opcode = memrpc::Opcode::ScanFile;
  first_call.admission_timeout_ms = 1000;
  first_call.queue_timeout_ms = 5000;
  first_call.exec_timeout_ms = 5000;
  first_call.payload = {0x31};
  auto first_future = client.InvokeAsync(first_call);
  ASSERT_TRUE(WaitForAtomicAtLeast(started, 1, 200));

  memrpc::RpcCall second_call;
  second_call.opcode = memrpc::Opcode::ScanFile;
  second_call.admission_timeout_ms = 1800;
  second_call.queue_timeout_ms = 10;
  second_call.exec_timeout_ms = 10;
  second_call.payload = {0x32};

  memrpc::StatusCode second_status = memrpc::StatusCode::EngineInternalError;
  memrpc::RpcReply second_reply;
  const auto start = std::chrono::steady_clock::now();
  std::thread sync_thread([&] { second_status = client.InvokeSync(second_call, &second_reply); });

  sync_thread.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_EQ(second_status, memrpc::StatusCode::QueueFull);
  EXPECT_GE(elapsed.count(), 1700);

  release.store(true);
  memrpc::RpcReply first_reply;
  EXPECT_EQ(first_future.Wait(&first_reply), memrpc::StatusCode::Ok);
  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, ResponseQueueFullRetriesUntilClientDrainsEvent) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 4,
          .normal_ring_size = 4,
          .response_ring_size = 1,
          .slot_count = 4,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&server](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           memrpc::RpcEvent event;
                           event.event_domain = 7;
                           event.event_type = 9;
                           event.payload = {0x33};
                           ASSERT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  client.SetEventCallback([&event_count](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 7u);
    EXPECT_EQ(event.event_type, 9u);
    ++event_count;
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{0x10, 0x20, 0x30};

  memrpc::RpcReply reply;
  EXPECT_EQ(client.InvokeSync(call, &reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(reply.payload, call.payload);
  EXPECT_EQ(event_count.load(), 1);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, BusyServerLeavesSharedRequestQueueBackedUp) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 2,
          .normal_ring_size = 4,
          .response_ring_size = 4,
          .slot_count = 4,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  std::atomic<int> started{0};
  std::atomic<bool> release{false};

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&started, &release](const memrpc::RpcServerCall& call,
                                              memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           started.fetch_add(1);
                           while (!release.load()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           }
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  std::vector<memrpc::RpcFuture> futures;
  for (int i = 0; i < 3; ++i) {
    memrpc::RpcCall call;
    call.opcode = memrpc::Opcode::ScanFile;
    call.admission_timeout_ms = 1000;
    call.queue_timeout_ms = 5000;
    call.exec_timeout_ms = 5000;
    call.payload = {static_cast<uint8_t>(i)};
    futures.push_back(client.InvokeAsync(call));
  }

  ASSERT_TRUE(WaitForAtomicAtLeast(started, 1, 200));

  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(bootstrap->server_handles(), memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);
  ASSERT_NE(observer.header(), nullptr);
  EXPECT_GT(memrpc::RingCount(observer.header()->normal_ring), 0u);

  release.store(true);
  for (int i = 0; i < 3; ++i) {
    memrpc::RpcReply reply;
    EXPECT_EQ(futures[i].Wait(&reply), memrpc::StatusCode::Ok);
  }

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, InvokeAsyncWaitsForSlotUntilAdmissionTimeout) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 2,
          .normal_ring_size = 2,
          .response_ring_size = 2,
          .slot_count = 1,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  std::atomic<int> started{0};
  std::atomic<bool> release{false};

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&started, &release](const memrpc::RpcServerCall& call,
                                              memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           started.fetch_add(1);
                           while (!release.load()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           }
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall first_call;
  first_call.opcode = memrpc::Opcode::ScanFile;
  first_call.admission_timeout_ms = 1000;
  first_call.queue_timeout_ms = 5000;
  first_call.exec_timeout_ms = 5000;
  first_call.payload = {1};
  auto first_future = client.InvokeAsync(first_call);
  ASSERT_TRUE(WaitForAtomicAtLeast(started, 1, 200));

  memrpc::RpcCall second_call;
  second_call.opcode = memrpc::Opcode::ScanFile;
  second_call.admission_timeout_ms = 200;
  second_call.queue_timeout_ms = 5000;
  second_call.exec_timeout_ms = 5000;
  second_call.payload = {2};

  std::atomic<memrpc::StatusCode> second_status{memrpc::StatusCode::EngineInternalError};
  std::thread second_thread([&] {
    memrpc::RpcReply second_reply;
    second_status.store(client.InvokeAsync(second_call).Wait(&second_reply));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  release.store(true);

  memrpc::RpcReply first_reply;
  EXPECT_EQ(first_future.Wait(&first_reply), memrpc::StatusCode::Ok);
  second_thread.join();
  EXPECT_EQ(second_status.load(), memrpc::StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, SharedMemoryShowsExecutingRequestWhileHandlerRuns) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  std::atomic<int> started{0};
  std::atomic<bool> release{false};

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&started, &release](const memrpc::RpcServerCall& call,
                                              memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           started.fetch_add(1);
                           while (!release.load()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           }
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.payload = {0x51, 0x52};
  auto future = client.InvokeAsync(call);

  ASSERT_TRUE(WaitForAtomicAtLeast(started, 1, 200));

  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(bootstrap->server_handles(), memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  bool saw_executing = false;
  for (uint32_t i = 0; i < observer.header()->slot_count; ++i) {
    const memrpc::SlotPayload* slot = observer.slot_payload(i);
    ASSERT_NE(slot, nullptr);
    if (slot->runtime.state == memrpc::SlotRuntimeStateCode::Executing) {
      saw_executing = true;
      EXPECT_NE(slot->runtime.request_id, 0u);
      EXPECT_GT(slot->runtime.start_exec_mono_ms, 0u);
      break;
    }
  }
  EXPECT_TRUE(saw_executing);

  release.store(true);
  memrpc::RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(reply.payload, call.payload);

  client.Shutdown();
  server.Stop();
}

TEST(RpcClientIntegrationTest, HighPriorityRequestStillAdmitsWhenNormalTrafficUsesNonReservedSlots) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 2,
          .normal_ring_size = 2,
          .response_ring_size = 4,
          .slot_count = 2,
          .high_reserved_request_slots = 1,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  std::atomic<int> started{0};
  std::atomic<bool> release{false};

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&started, &release](const memrpc::RpcServerCall& call,
                                              memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           started.fetch_add(1);
                           while (!release.load()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(1));
                           }
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall normal_call;
  normal_call.opcode = memrpc::Opcode::ScanFile;
  normal_call.priority = memrpc::Priority::Normal;
  normal_call.admission_timeout_ms = 150;
  normal_call.queue_timeout_ms = 5000;
  normal_call.exec_timeout_ms = 5000;
  normal_call.payload = {0x11};
  auto first_normal = client.InvokeAsync(normal_call);
  ASSERT_TRUE(WaitForAtomicAtLeast(started, 1, 200));

  memrpc::RpcReply blocked_normal_reply;
  const memrpc::StatusCode blocked_normal_status = client.InvokeAsync(normal_call).Wait(&blocked_normal_reply);
  EXPECT_EQ(blocked_normal_status, memrpc::StatusCode::QueueFull);

  memrpc::RpcCall high_call = normal_call;
  high_call.priority = memrpc::Priority::High;
  high_call.payload = {0x22};

  std::atomic<memrpc::StatusCode> high_status{memrpc::StatusCode::EngineInternalError};
  std::thread high_thread([&] {
    memrpc::RpcReply high_reply;
    high_status.store(client.InvokeAsync(high_call).Wait(&high_reply));
    if (high_status.load() == memrpc::StatusCode::Ok) {
      EXPECT_EQ(high_reply.payload, high_call.payload);
    }
  });

  ASSERT_TRUE(WaitForAtomicAtLeast(started, 2, 500));
  release.store(true);

  memrpc::RpcReply first_normal_reply;
  EXPECT_EQ(first_normal.Wait(&first_normal_reply), memrpc::StatusCode::Ok);
  high_thread.join();
  EXPECT_EQ(high_status.load(), memrpc::StatusCode::Ok);

  client.Shutdown();
  server.Stop();
}
