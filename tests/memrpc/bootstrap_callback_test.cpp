#include <gtest/gtest.h>

#include <atomic>
#include <unistd.h>
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/client/demo_bootstrap.h"

namespace {

void CloseHandles(memrpc::BootstrapHandles& h) {
    if (h.shm_fd >= 0) close(h.shm_fd);
    if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
    if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
    if (h.resp_event_fd >= 0) close(h.resp_event_fd);
    if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
    if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

}  // namespace

TEST(BootstrapCallbackTest, InvokesEngineDeathCallback) {
  memrpc::SaBootstrapChannel channel;
  std::atomic<int> callback_count{0};
  std::atomic<uint64_t> dead_session_id{0};

  channel.SetEngineDeathCallback([&](uint64_t session_id) {
    dead_session_id.store(session_id);
    callback_count.fetch_add(1);
  });
  memrpc::BootstrapHandles handles;
  ASSERT_EQ(channel.OpenSession(&handles), memrpc::StatusCode::Ok);

  channel.SimulateEngineDeathForTest();
  EXPECT_EQ(callback_count.load(), 1);
  EXPECT_EQ(dead_session_id.load(), handles.session_id);

  CloseHandles(handles);
}

TEST(BootstrapCallbackTest, RestartProducesNewSessionId) {
  memrpc::SaBootstrapChannel channel;
  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(channel.OpenSession(&first_handles), memrpc::StatusCode::Ok);

  channel.SimulateEngineDeathForTest();

  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(channel.OpenSession(&second_handles), memrpc::StatusCode::Ok);
  EXPECT_NE(first_handles.session_id, second_handles.session_id);

  CloseHandles(first_handles);
  CloseHandles(second_handles);
}

TEST(BootstrapCallbackTest, CanDeliverLateDeathForSpecificSessionWithoutDroppingCurrentHandles) {
  memrpc::SaBootstrapChannel channel;
  std::atomic<uint64_t> dead_session_id{0};

  channel.SetEngineDeathCallback(
      [&](uint64_t session_id) { dead_session_id.store(session_id); });
  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(channel.OpenSession(&first_handles), memrpc::StatusCode::Ok);
  channel.SimulateEngineDeathForTest();

  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(channel.OpenSession(&second_handles), memrpc::StatusCode::Ok);
  ASSERT_NE(first_handles.session_id, second_handles.session_id);

  dead_session_id.store(0);
  channel.SimulateEngineDeathForTest(first_handles.session_id);
  EXPECT_EQ(dead_session_id.load(), first_handles.session_id);

  memrpc::BootstrapHandles latest_handles;
  ASSERT_EQ(channel.OpenSession(&latest_handles), memrpc::StatusCode::Ok);
  EXPECT_EQ(latest_handles.session_id, second_handles.session_id);

  CloseHandles(first_handles);
  CloseHandles(second_handles);
  CloseHandles(latest_handles);
}

TEST(BootstrapCallbackTest, OpenSessionFailsWhenHandleDuplicationFails) {
  memrpc::PosixDemoBootstrapChannel channel;
  channel.SetDupFailureAfterCountForTest(2);

  memrpc::BootstrapHandles handles;
  const memrpc::StatusCode status = channel.OpenSession(&handles);

  EXPECT_EQ(status, memrpc::StatusCode::EngineInternalError);
  EXPECT_EQ(handles.shm_fd, -1);
  EXPECT_EQ(handles.high_req_event_fd, -1);
  EXPECT_EQ(handles.normal_req_event_fd, -1);
  EXPECT_EQ(handles.resp_event_fd, -1);
  EXPECT_EQ(handles.req_credit_event_fd, -1);
  EXPECT_EQ(handles.resp_credit_event_fd, -1);
}
