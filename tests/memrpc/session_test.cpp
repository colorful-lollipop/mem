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
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
}

}  // namespace

TEST(SessionTest, AttachRejectsInvalidHeaderLayout) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles corrupt_handles;
  ASSERT_EQ(bootstrap->OpenSession(corrupt_handles), memrpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shmFd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shmFd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<memrpc::SharedMemoryHeader*>(region);
  header->slotSize = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  memrpc::BootstrapHandles attach_handles;
  ASSERT_EQ(bootstrap->OpenSession(attach_handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), memrpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, AttachRejectsProtocolVersionMismatch) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles corrupt_handles;
  ASSERT_EQ(bootstrap->OpenSession(corrupt_handles), memrpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shmFd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shmFd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<memrpc::SharedMemoryHeader*>(region);
  header->protocolVersion = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  memrpc::BootstrapHandles attach_handles;
  ASSERT_EQ(bootstrap->OpenSession(attach_handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), memrpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, DefaultsToFourKilobytePayloadLimits) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);
  ASSERT_NE(session.Header(), nullptr);
  EXPECT_EQ(session.Header()->maxRequestBytes, 4096u);
  EXPECT_EQ(session.Header()->maxResponseBytes, 4096u);
}

TEST(SessionTest, RequestRingsWrapAroundWithoutLosingCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.highRingSize = 2;
  config.normalRingSize = 2;
  config.responseRingSize = 2;
  config.slotCount = 2;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry first;
  first.requestId = 1;
  first.slotIndex = 0;
  memrpc::RequestRingEntry second;
  second.requestId = 2;
  second.slotIndex = 1;
  memrpc::RequestRingEntry third;
  third.requestId = 3;
  third.slotIndex = 0;

  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, first),
            memrpc::StatusCode::Ok);
  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, second),
            memrpc::StatusCode::Ok);
  EXPECT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, third),
            memrpc::StatusCode::QueueFull);

  memrpc::RequestRingEntry observed;
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 1u);

  ASSERT_EQ(client_session.PushRequest(memrpc::QueueKind::NormalRequest, third),
            memrpc::StatusCode::Ok);
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 2u);
  ASSERT_TRUE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 3u);
  EXPECT_FALSE(server_session.PopRequest(memrpc::QueueKind::NormalRequest, &observed));
}

TEST(SessionTest, ResponseRingWrapsAroundWithoutLosingCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.highRingSize = 2;
  config.normalRingSize = 2;
  config.responseRingSize = 2;
  config.slotCount = 2;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  memrpc::ResponseRingEntry first;
  first.requestId = 11;
  memrpc::ResponseRingEntry second;
  second.requestId = 12;
  memrpc::ResponseRingEntry third;
  third.requestId = 13;

  ASSERT_EQ(server_session.PushResponse(first), memrpc::StatusCode::Ok);
  ASSERT_EQ(server_session.PushResponse(second), memrpc::StatusCode::Ok);
  EXPECT_EQ(server_session.PushResponse(third), memrpc::StatusCode::QueueFull);

  memrpc::ResponseRingEntry observed;
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 11u);

  ASSERT_EQ(server_session.PushResponse(third), memrpc::StatusCode::Ok);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 12u);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 13u);
  EXPECT_FALSE(client_session.PopResponse(&observed));
}

TEST(SessionTest, ResponsePayloadLimitCannotExceedInlineQueueCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.maxResponseBytes = memrpc::DEFAULT_MAX_RESPONSE_BYTES + 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  memrpc::BootstrapHandles invalid_handles;
  EXPECT_EQ(bootstrap->OpenSession(invalid_handles), memrpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, PushRequestReturnsQueueFullWhenRingIsAtCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.highRingSize = 1;
  config.normalRingSize = 1;
  config.responseRingSize = 1;
  config.slotCount = 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry first;
  first.requestId = 1;
  first.slotIndex = 0;
  EXPECT_EQ(session.PushRequest(memrpc::QueueKind::NormalRequest, first), memrpc::StatusCode::Ok);

  memrpc::RequestRingEntry second;
  second.requestId = 2;
  second.slotIndex = 0;
  EXPECT_EQ(session.PushRequest(memrpc::QueueKind::NormalRequest, second),
            memrpc::StatusCode::QueueFull);
}

TEST(SessionTest, RejectsSecondClientAttachToSameSession) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(bootstrap->OpenSession(first_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(bootstrap->OpenSession(second_handles), memrpc::StatusCode::Ok);

  memrpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), memrpc::StatusCode::Ok);

  memrpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), memrpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, AllowsNextClientAttachAfterReset) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(bootstrap->OpenSession(first_handles), memrpc::StatusCode::Ok);

  memrpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), memrpc::StatusCode::Ok);
  first_session.Reset();

  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(bootstrap->OpenSession(second_handles), memrpc::StatusCode::Ok);

  memrpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), memrpc::StatusCode::Ok);
}

TEST(SessionTest, SlotRuntimeStateDefaultsAreZeroed) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  ASSERT_EQ(session.Attach(handles), memrpc::StatusCode::Ok);

  const memrpc::SlotPayload* slot = session.GetSlotPayload(0);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->runtime.requestId, 0u);
  EXPECT_EQ(slot->runtime.state, memrpc::SlotRuntimeStateCode::Free);
  EXPECT_EQ(slot->runtime.workerId, 0u);
  EXPECT_EQ(slot->runtime.enqueueMonoMs, 0u);
  EXPECT_EQ(slot->runtime.startExecMonoMs, 0u);
  EXPECT_EQ(slot->runtime.lastHeartbeatMonoMs, 0u);

  const memrpc::ResponseSlotPayload* response_slot = session.GetResponseSlotPayload(0);
  ASSERT_NE(response_slot, nullptr);
  EXPECT_EQ(response_slot->runtime.requestId, 0u);
  EXPECT_EQ(response_slot->runtime.state, memrpc::SlotRuntimeStateCode::Free);
}

TEST(SessionTest, AttachPreservesCreditEventFds) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();

  memrpc::BootstrapHandles client_handles;
  ASSERT_EQ(bootstrap->OpenSession(client_handles), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles server_handles;
  ASSERT_EQ(bootstrap->OpenSession(server_handles), memrpc::StatusCode::Ok);

  memrpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), memrpc::StatusCode::Ok);
  memrpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, memrpc::Session::AttachRole::Server),
            memrpc::StatusCode::Ok);

  EXPECT_GE(client_session.Handles().reqCreditEventFd, 0);
  EXPECT_GE(client_session.Handles().respCreditEventFd, 0);
  EXPECT_GE(server_session.Handles().reqCreditEventFd, 0);
  EXPECT_GE(server_session.Handles().respCreditEventFd, 0);
}
