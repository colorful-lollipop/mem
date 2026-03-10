#include <gtest/gtest.h>

#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "core/session.h"
#include "memrpc/client/demo_bootstrap.h"

namespace {

bool WaitForExit(pid_t pid, int timeout_ms, int* status) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t wait_result = waitpid(pid, status, WNOHANG);
    if (wait_result == pid) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void CloseHandles(memrpc::BootstrapHandles& h) {
  if (h.shm_fd >= 0) close(h.shm_fd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

}  // namespace

TEST(SessionTest, AttachRejectsInvalidHeaderLayout) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles corrupt_handles;
  ASSERT_EQ(bootstrap->OpenSession(&corrupt_handles), memrpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shm_fd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shm_fd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<memrpc::SharedMemoryHeader*>(region);
  header->slot_size = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  memrpc::BootstrapHandles attach_handles;
  ASSERT_EQ(bootstrap->OpenSession(&attach_handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), memrpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, AttachRejectsProtocolVersionMismatch) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles corrupt_handles;
  ASSERT_EQ(bootstrap->OpenSession(&corrupt_handles), memrpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shm_fd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shm_fd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<memrpc::SharedMemoryHeader*>(region);
  header->protocol_version = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  memrpc::BootstrapHandles attach_handles;
  ASSERT_EQ(bootstrap->OpenSession(&attach_handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), memrpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, DefaultsToFourKilobytePayloadLimits) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(&handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);
  ASSERT_NE(session.header(), nullptr);
  EXPECT_EQ(session.header()->max_request_bytes, 4096u);
  EXPECT_EQ(session.header()->max_response_bytes, 4096u);
}

TEST(SessionTest, RequestRingsWrapAroundWithoutLosingCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.high_ring_size = 2;
  config.normal_ring_size = 2;
  config.response_ring_size = 2;
  config.slot_count = 2;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(&client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(&server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry first;
  first.request_id = 1;
  first.slot_index = 0;
  memrpc::RequestRingEntry second;
  second.request_id = 2;
  second.slot_index = 1;
  memrpc::RequestRingEntry third;
  third.request_id = 3;
  third.slot_index = 0;

  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, first),
            memrpc::StatusCode::Ok);
  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, second),
            memrpc::StatusCode::Ok);
  EXPECT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, third),
            memrpc::StatusCode::QueueFull);

  memrpc::RequestRingEntry observed;
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.request_id, 1u);

  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, third),
            memrpc::StatusCode::Ok);
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.request_id, 2u);
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.request_id, 3u);
  EXPECT_FALSE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
}

TEST(SessionTest, ResponseRingWrapsAroundWithoutLosingCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.high_ring_size = 2;
  config.normal_ring_size = 2;
  config.response_ring_size = 2;
  config.slot_count = 2;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(&client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(&server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::ResponseRingEntry first;
  first.request_id = 11;
  memrpc::ResponseRingEntry second;
  second.request_id = 12;
  memrpc::ResponseRingEntry third;
  third.request_id = 13;

  ASSERT_EQ(server_session.PushResponse(first), memrpc::StatusCode::Ok);
  ASSERT_EQ(server_session.PushResponse(second), memrpc::StatusCode::Ok);
  EXPECT_EQ(server_session.PushResponse(third), memrpc::StatusCode::QueueFull);

  memrpc::ResponseRingEntry observed;
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.request_id, 11u);

  ASSERT_EQ(server_session.PushResponse(third), memrpc::StatusCode::Ok);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.request_id, 12u);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.request_id, 13u);
  EXPECT_FALSE(client_session.PopResponse(&observed));
}

TEST(SessionTest, ResponsePayloadLimitCannotExceedInlineQueueCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.max_response_bytes = memrpc::kDefaultMaxResponseBytes + 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  memrpc::BootstrapHandles invalid_handles;
  EXPECT_EQ(bootstrap->OpenSession(&invalid_handles), memrpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, PushRequestReturnsQueueFullWhenRingIsAtCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.high_ring_size = 1;
  config.normal_ring_size = 1;
  config.response_ring_size = 1;
  config.slot_count = 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(&handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry first;
  first.request_id = 1;
  first.slot_index = 0;
  EXPECT_EQ(session.PushRequest(memrpc::QueueKind::NormalRequest, first), memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry second;
  second.request_id = 2;
  second.slot_index = 0;
  EXPECT_EQ(session.PushRequest(memrpc::QueueKind::NormalRequest, second),
            memrpc::StatusCode::QueueFull);
}

TEST(SessionTest, RejectsSecondClientAttachToSameSession) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(bootstrap->OpenSession(&first_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(bootstrap->OpenSession(&second_handles), memrpc::StatusCode::Ok);

  memrpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), memrpc::StatusCode::Ok);

  memrpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), memrpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, AllowsNextClientAttachAfterReset) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(bootstrap->OpenSession(&first_handles), memrpc::StatusCode::Ok);

  memrpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), memrpc::StatusCode::Ok);
  first_session.Reset();

  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(bootstrap->OpenSession(&second_handles), memrpc::StatusCode::Ok);

  memrpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), memrpc::StatusCode::Ok);
}

TEST(SessionTest, SlotRuntimeStateDefaultsAreZeroed) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(&handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);

  const memrpc::SlotPayload* slot = session.slot_payload(0);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->runtime.request_id, 0u);
  EXPECT_EQ(slot->runtime.state, memrpc::SlotRuntimeStateCode::Free);
  EXPECT_EQ(slot->runtime.worker_id, 0u);
  EXPECT_EQ(slot->runtime.enqueue_mono_ms, 0u);
  EXPECT_EQ(slot->runtime.start_exec_mono_ms, 0u);
  EXPECT_EQ(slot->runtime.last_heartbeat_mono_ms, 0u);

  const memrpc::ResponseSlotPayload* response_slot = session.response_slot_payload(0);
  ASSERT_NE(response_slot, nullptr);
  EXPECT_EQ(response_slot->runtime.request_id, 0u);
  EXPECT_EQ(response_slot->runtime.state, memrpc::SlotRuntimeStateCode::Free);
}

TEST(SessionTest, AttachPreservesCreditEventFds) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(&client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(&server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  EXPECT_GE(client_session.handles().req_credit_event_fd, 0);
  EXPECT_GE(client_session.handles().resp_credit_event_fd, 0);
  EXPECT_GE(server_session.handles().req_credit_event_fd, 0);
  EXPECT_GE(server_session.handles().resp_credit_event_fd, 0);
}
