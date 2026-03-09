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

}  // namespace

TEST(SessionTest, AttachRejectsInvalidHeaderLayout) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles corrupt_handles;
  ASSERT_EQ(bootstrap->Connect(&corrupt_handles), memrpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shm_fd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shm_fd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<memrpc::SharedMemoryHeader*>(region);
  header->slot_size = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  close(corrupt_handles.shm_fd);
  close(corrupt_handles.high_req_event_fd);
  close(corrupt_handles.normal_req_event_fd);
  close(corrupt_handles.resp_event_fd);

  memrpc::BootstrapHandles attach_handles;
  ASSERT_EQ(bootstrap->Connect(&attach_handles), memrpc::StatusCode::Ok);

  memrpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), memrpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, OwnerDeathDoesNotHangRingOperations) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  const pid_t locker_pid = fork();
  ASSERT_GE(locker_pid, 0);
  if (locker_pid == 0) {
    memrpc::BootstrapHandles locker_handles;
    if (bootstrap->Connect(&locker_handles) != memrpc::StatusCode::Ok) {
      _exit(2);
    }
    memrpc::Session session;
    if (session.Attach(locker_handles) != memrpc::StatusCode::Ok) {
      _exit(3);
    }
    if (pthread_mutex_lock(&session.mutable_header()->high_ring_mutex) != 0) {
      _exit(4);
    }
    _exit(0);
  }

  int locker_status = 0;
  ASSERT_EQ(waitpid(locker_pid, &locker_status, 0), locker_pid);
  ASSERT_TRUE(WIFEXITED(locker_status));
  ASSERT_EQ(WEXITSTATUS(locker_status), 0);

  const pid_t probe_pid = fork();
  ASSERT_GE(probe_pid, 0);
  if (probe_pid == 0) {
    memrpc::BootstrapHandles probe_handles;
    if (bootstrap->Connect(&probe_handles) != memrpc::StatusCode::Ok) {
      _exit(5);
    }
    memrpc::Session session;
    if (session.Attach(probe_handles) != memrpc::StatusCode::Ok) {
      _exit(6);
    }
    memrpc::RequestRingEntry entry;
    entry.request_id = 1;
    entry.slot_index = 0;
    const memrpc::StatusCode status =
        session.PushRequest(memrpc::QueueKind::HighRequest, entry);
    _exit(status == memrpc::StatusCode::PeerDisconnected ? 0 : 7);
  }

  int probe_status = 0;
  if (!WaitForExit(probe_pid, 1000, &probe_status)) {
    kill(probe_pid, SIGKILL);
    waitpid(probe_pid, &probe_status, 0);
    FAIL() << "PushRequest hung after mutex owner death";
  }
  ASSERT_TRUE(WIFEXITED(probe_status));
  EXPECT_EQ(WEXITSTATUS(probe_status), 0);
}

TEST(SessionTest, ResponsePayloadLimitCannotExceedInlineQueueCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.max_response_bytes = memrpc::kDefaultMaxResponseBytes + 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  EXPECT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, PushRequestReturnsQueueFullWhenRingIsAtCapacity) {
  memrpc::DemoBootstrapConfig config;
  config.high_ring_size = 1;
  config.normal_ring_size = 1;
  config.response_ring_size = 1;
  config.slot_count = 1;

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(config);
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(bootstrap->Connect(&handles), memrpc::StatusCode::Ok);

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
