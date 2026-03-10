#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/session.h"
#include "core/session_test_hook.h"
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

uint64_t DrainEventFdCounter(int fd) {
  uint64_t counter = 0;
  const ssize_t rc = read(fd, &counter, sizeof(counter));
  if (rc == static_cast<ssize_t>(sizeof(counter))) {
    return counter;
  }
  EXPECT_TRUE(rc < 0 && errno == EAGAIN);
  return 0;
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

TEST(ResponseQueueEventTest, ResponseEventFdSignalsOnlyOnEmptyToNonEmptyTransition) {
  memrpc::DemoBootstrapConfig config;
  config.response_ring_size = 4;
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

  memrpc::BootstrapHandles observer_handles = bootstrap->server_handles();
  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(observer_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::RpcEvent first;
  first.event_domain = 1;
  first.event_type = 11;
  first.payload = {1};
  memrpc::RpcEvent second = first;
  second.event_type = 12;
  second.payload = {2};

  ASSERT_EQ(server.PublishEvent(first), memrpc::StatusCode::Ok);
  ASSERT_EQ(server.PublishEvent(second), memrpc::StatusCode::Ok);

  ASSERT_TRUE(WaitForCondition(
      [&observer] { return memrpc::RingCount(observer.header()->response_ring) == 2; }, 200));
  EXPECT_EQ(DrainEventFdCounter(observer.handles().resp_event_fd), 1u);

  server.Stop();
}

TEST(ResponseQueueEventTest, ClientSignalsResponseCreditWhenFullResponseQueueStartsDraining) {
  memrpc::DemoBootstrapConfig config;
  config.response_ring_size = 2;
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

  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(bootstrap->server_handles(), memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::RpcEvent first;
  first.event_domain = 9;
  first.event_type = 1;
  first.payload = {0x11};
  memrpc::RpcEvent second = first;
  second.event_type = 2;
  second.payload = {0x22};

  ASSERT_EQ(server.PublishEvent(first), memrpc::StatusCode::Ok);
  ASSERT_EQ(server.PublishEvent(second), memrpc::StatusCode::Ok);
  ASSERT_TRUE(WaitForCondition(
      [&observer] { return memrpc::RingCount(observer.header()->response_ring) == 2; }, 200));

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  client.SetEventCallback([&](const memrpc::RpcEvent&) { event_count.fetch_add(1); });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  ASSERT_TRUE(WaitForValue(event_count, 2, 500));
  EXPECT_EQ(DrainEventFdCounter(observer.handles().resp_credit_event_fd), 1u);

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, ResponseWriterConsumesManualResponseCreditWakeup) {
  memrpc::DemoBootstrapConfig config;
  config.response_ring_size = 1;
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->Connect(&client_handles), memrpc::StatusCode::Ok);
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);

  memrpc::RpcEvent first;
  first.event_domain = 3;
  first.event_type = 1;
  first.payload = {0x41};
  memrpc::RpcEvent second = first;
  second.event_type = 2;
  second.payload = {0x42};

  ASSERT_EQ(server.PublishEvent(first), memrpc::StatusCode::Ok);

  std::atomic<bool> second_done{false};
  std::thread publisher([&] {
    EXPECT_EQ(server.PublishEvent(second), memrpc::StatusCode::Ok);
    second_done.store(true);
  });

  ASSERT_TRUE(WaitForCondition(
      [&server] { return server.GetRuntimeStats().waiting_for_response_credit; }, 200));

  memrpc::ResponseRingEntry entry;
  ASSERT_TRUE(client_session.PopResponse(&entry));
  memrpc::SharedSlotPool response_slot_pool(client_session.response_slot_pool_region());
  ASSERT_TRUE(response_slot_pool.valid());
  ASSERT_TRUE(response_slot_pool.Release(entry.slot_index));

  const uint64_t signal_value = 1;
  ASSERT_EQ(write(client_session.handles().resp_credit_event_fd, &signal_value,
                  sizeof(signal_value)),
            static_cast<ssize_t>(sizeof(signal_value)));

  ASSERT_TRUE(WaitForCondition([&second_done] { return second_done.load(); }, 200));
  publisher.join();

  EXPECT_EQ(DrainEventFdCounter(client_session.handles().resp_credit_event_fd), 0u);

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

TEST(ResponseQueueEventTest, InterleavedRepliesAndEventsPreserveBothCounts) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 2, .normal_worker_threads = 2});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&server](const memrpc::RpcServerCall& call,
                                   memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           memrpc::RpcEvent event;
                           event.event_domain = 9;
                           event.event_type = static_cast<uint32_t>(call.payload.front());
                           event.payload = call.payload;
                           ASSERT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  client.SetEventCallback([&](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 9u);
    ++event_count;
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  constexpr int kRequestCount = 16;
  for (int i = 0; i < kRequestCount; ++i) {
    memrpc::RpcCall call;
    call.opcode = memrpc::Opcode::ScanFile;
    call.payload = {static_cast<uint8_t>(i)};

    memrpc::RpcReply reply;
    ASSERT_EQ(client.InvokeAsync(call).Wait(&reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.payload, call.payload);
  }

  ASSERT_TRUE(WaitForValue(event_count, kRequestCount, 500));

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, ResponseRingUsesSingleProducerAndConsumerThreads) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 2, .normal_worker_threads = 2});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [&server](const memrpc::RpcServerCall& call,
                                   memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           memrpc::RpcEvent event;
                           event.event_domain = 42;
                           event.event_type = static_cast<uint32_t>(call.payload.front());
                           event.payload = {0xee, call.payload.front()};
                           ASSERT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  std::atomic<int> event_count{0};
  client.SetEventCallback([&event_count](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 42u);
    ++event_count;
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  RingTraceRecorder recorder;
  std::vector<std::thread> threads;
  constexpr int kRequestCount = 12;
  for (int i = 0; i < kRequestCount; ++i) {
    threads.emplace_back([&client, i] {
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.payload = {static_cast<uint8_t>(i)};
      memrpc::RpcReply reply;
      EXPECT_EQ(client.InvokeAsync(call).Wait(&reply), memrpc::StatusCode::Ok);
      EXPECT_EQ(reply.payload, call.payload);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(WaitForValue(event_count, kRequestCount, 500));
  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PushResponse), 1u);
  EXPECT_EQ(recorder.UniqueThreadCount(memrpc::RingTraceOperation::PopResponse), 1u);

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, EventCallbackSeesReadyResponseSlotBeforeRelease) {
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
  std::atomic<int> callback_started{0};
  std::atomic<bool> release_callback{false};
  client.SetEventCallback([&](const memrpc::RpcEvent& event) {
    EXPECT_EQ(event.event_domain, 88u);
    callback_started.fetch_add(1);
    while (!release_callback.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcEvent event;
  event.event_domain = 88;
  event.event_type = 99;
  event.payload = {0x31, 0x32};

  std::thread publisher([&] {
    EXPECT_EQ(server.PublishEvent(event), memrpc::StatusCode::Ok);
  });

  ASSERT_TRUE(WaitForValue(callback_started, 1, 500));

  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(bootstrap->server_handles(), memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  bool saw_ready = false;
  for (uint32_t i = 0; i < observer.header()->response_ring_size; ++i) {
    const memrpc::ResponseSlotPayload* slot = observer.response_slot_payload(i);
    ASSERT_NE(slot, nullptr);
    if (slot->runtime.state == memrpc::SlotRuntimeStateCode::Ready) {
      saw_ready = true;
      EXPECT_EQ(slot->response.payload_size, event.payload.size());
      break;
    }
  }
  EXPECT_TRUE(saw_ready);

  release_callback.store(true);
  publisher.join();

  client.Shutdown();
  server.Stop();
}

TEST(ResponseQueueEventTest, PublishedEventKeepsResponseSlotOwnedWhenNotifyWriteFails) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 2,
          .normal_ring_size = 2,
          .response_ring_size = 2,
          .slot_count = 2,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
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

  memrpc::BootstrapHandles fill_handles = bootstrap->server_handles();
  const uint64_t saturated_counter = UINT64_MAX - 1;
  ASSERT_EQ(write(fill_handles.resp_event_fd, &saturated_counter, sizeof(saturated_counter)),
            static_cast<ssize_t>(sizeof(saturated_counter)));

  memrpc::RpcEvent event;
  event.event_domain = 55;
  event.event_type = 66;
  event.payload = {0x41, 0x42, 0x43};
  EXPECT_EQ(server.PublishEvent(event), memrpc::StatusCode::PeerDisconnected);

  memrpc::BootstrapHandles observer_handles = bootstrap->server_handles();
  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(observer_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::SharedSlotPool response_slot_pool(observer.response_slot_pool_region());
  ASSERT_TRUE(response_slot_pool.valid());
  EXPECT_EQ(response_slot_pool.available(), observer.header()->response_ring_size - 1);

  memrpc::ResponseRingEntry entry;
  ASSERT_TRUE(observer.PopResponse(&entry));
  const memrpc::ResponseSlotPayload* slot = observer.response_slot_payload(entry.slot_index);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->runtime.state, memrpc::SlotRuntimeStateCode::Ready);
  EXPECT_EQ(slot->response.payload_size, event.payload.size());

  close(fill_handles.shm_fd);
  close(fill_handles.high_req_event_fd);
  close(fill_handles.normal_req_event_fd);
  close(fill_handles.resp_event_fd);

  server.Stop();
}

TEST(ResponseQueueEventTest, ReplyKeepsResponseSlotOwnedWhenNotifyWriteFails) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 2,
          .normal_ring_size = 2,
          .response_ring_size = 2,
          .slot_count = 2,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = {0x11, 0x22};
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles fill_handles = bootstrap->server_handles();
  const uint64_t saturated_counter = UINT64_MAX - 1;
  ASSERT_EQ(write(fill_handles.resp_event_fd, &saturated_counter, sizeof(saturated_counter)),
            static_cast<ssize_t>(sizeof(saturated_counter)));

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->Connect(&client_handles), memrpc::StatusCode::Ok);
  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);

  memrpc::SlotPayload* payload = client_session.slot_payload(0);
  uint8_t* request_bytes = client_session.slot_request_bytes(0);
  ASSERT_NE(payload, nullptr);
  ASSERT_NE(request_bytes, nullptr);
  std::memset(payload, 0, sizeof(memrpc::SlotPayload));
  payload->runtime.request_id = 1;
  payload->runtime.state = memrpc::SlotRuntimeStateCode::Queued;
  payload->request.opcode = static_cast<uint16_t>(memrpc::Opcode::ScanFile);
  payload->request.queue_timeout_ms = 0;
  payload->request.exec_timeout_ms = 0;
  payload->request.payload_size = 1;
  request_bytes[0] = 0x33;

  memrpc::RequestRingEntry request_entry;
  request_entry.request_id = 1;
  request_entry.slot_index = 0;
  request_entry.opcode = static_cast<uint16_t>(memrpc::Opcode::ScanFile);
  request_entry.payload_size = 1;
  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, request_entry),
            memrpc::StatusCode::Ok);
  const uint64_t signal_value = 1;
  ASSERT_EQ(write(client_session.handles().normal_req_event_fd, &signal_value,
                  sizeof(signal_value)),
            static_cast<ssize_t>(sizeof(signal_value)));

  memrpc::BootstrapHandles observer_handles = bootstrap->server_handles();
  memrpc::Session observer;
  ASSERT_EQ(observer.Attach(observer_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);
  ASSERT_TRUE(WaitForCondition(
      [&observer] { return memrpc::RingCount(observer.header()->response_ring) == 1; }, 200));

  memrpc::SharedSlotPool response_slot_pool(observer.response_slot_pool_region());
  ASSERT_TRUE(response_slot_pool.valid());
  EXPECT_EQ(response_slot_pool.available(), observer.header()->response_ring_size - 1);

  memrpc::ResponseRingEntry entry;
  ASSERT_TRUE(observer.PopResponse(&entry));
  const memrpc::ResponseSlotPayload* slot = observer.response_slot_payload(entry.slot_index);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->runtime.state, memrpc::SlotRuntimeStateCode::Ready);

  close(fill_handles.shm_fd);
  close(fill_handles.high_req_event_fd);
  close(fill_handles.normal_req_event_fd);
  close(fill_handles.resp_event_fd);
  close(client_handles.shm_fd);
  close(client_handles.high_req_event_fd);
  close(client_handles.normal_req_event_fd);
  close(client_handles.resp_event_fd);
  close(client_handles.req_credit_event_fd);
  close(client_handles.resp_credit_event_fd);

  server.Stop();
}

TEST(ResponseQueueEventTest, PublishEventFailsFastWhenCompletionBacklogIsFull) {
  auto bootstrap =
      std::make_shared<memrpc::PosixDemoBootstrapChannel>(memrpc::DemoBootstrapConfig{
          .high_ring_size = 1,
          .normal_ring_size = 1,
          .response_ring_size = 1,
          .slot_count = 1,
          .max_request_bytes = memrpc::kDefaultMaxRequestBytes,
          .max_response_bytes = memrpc::kDefaultMaxResponseBytes,
      });
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.SetOptions({.high_worker_threads = 1, .normal_worker_threads = 1, .completion_queue_capacity = 1});
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcEvent first_event;
  first_event.event_domain = 1;
  first_event.event_type = 1;
  first_event.payload = {0x11};
  ASSERT_EQ(server.PublishEvent(first_event), memrpc::StatusCode::Ok);

  memrpc::RpcEvent queued_event;
  queued_event.event_domain = 2;
  queued_event.event_type = 2;
  queued_event.payload = {0x22};

  memrpc::StatusCode slow_status = memrpc::StatusCode::Ok;
  auto slow_elapsed = std::chrono::milliseconds::zero();
  std::thread blocked_publisher([&] {
    const auto start = std::chrono::steady_clock::now();
    slow_status = server.PublishEvent(queued_event);
    slow_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  const memrpc::RpcServerRuntimeStats stats = server.GetRuntimeStats();
  EXPECT_EQ(stats.completion_backlog, 1u);
  EXPECT_EQ(stats.completion_backlog_capacity, 1u);

  memrpc::RpcEvent overflow_event;
  overflow_event.event_domain = 3;
  overflow_event.event_type = 3;
  overflow_event.payload = {0x33};

  const auto fast_start = std::chrono::steady_clock::now();
  const memrpc::StatusCode fast_status = server.PublishEvent(overflow_event);
  const auto fast_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            fast_start);

  blocked_publisher.join();
  EXPECT_EQ(slow_status, memrpc::StatusCode::QueueFull);
  EXPECT_EQ(fast_status, memrpc::StatusCode::QueueFull);
  EXPECT_GE(slow_elapsed.count(), 8);
  EXPECT_LT(fast_elapsed.count(), 8);

  server.Stop();
}
