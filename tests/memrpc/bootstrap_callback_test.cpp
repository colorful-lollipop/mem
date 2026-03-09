#include <gtest/gtest.h>

#include <atomic>
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/client/demo_bootstrap.h"

TEST(BootstrapCallbackTest, InvokesEngineDeathCallback) {
  memrpc::SaBootstrapChannel channel;
  std::atomic<int> callback_count{0};
  std::atomic<uint64_t> dead_session_id{0};

  channel.SetEngineDeathCallback([&](uint64_t session_id) {
    dead_session_id.store(session_id);
    callback_count.fetch_add(1);
  });
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles handles;
  ASSERT_EQ(channel.Connect(&handles), memrpc::StatusCode::Ok);

  channel.SimulateEngineDeathForTest();
  EXPECT_EQ(callback_count.load(), 1);
  EXPECT_EQ(dead_session_id.load(), handles.session_id);

  close(handles.shm_fd);
  close(handles.high_req_event_fd);
  close(handles.normal_req_event_fd);
  close(handles.resp_event_fd);
  close(handles.req_credit_event_fd);
  close(handles.resp_credit_event_fd);
}

TEST(BootstrapCallbackTest, RestartProducesNewSessionId) {
  memrpc::SaBootstrapChannel channel;
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(channel.Connect(&first_handles), memrpc::StatusCode::Ok);

  channel.SimulateEngineDeathForTest();
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(channel.Connect(&second_handles), memrpc::StatusCode::Ok);
  EXPECT_NE(first_handles.session_id, second_handles.session_id);

  close(first_handles.shm_fd);
  close(first_handles.high_req_event_fd);
  close(first_handles.normal_req_event_fd);
  close(first_handles.resp_event_fd);
  close(first_handles.req_credit_event_fd);
  close(first_handles.resp_credit_event_fd);
  close(second_handles.shm_fd);
  close(second_handles.high_req_event_fd);
  close(second_handles.normal_req_event_fd);
  close(second_handles.resp_event_fd);
  close(second_handles.req_credit_event_fd);
  close(second_handles.resp_credit_event_fd);
}

TEST(BootstrapCallbackTest, CanDeliverLateDeathForSpecificSessionWithoutDroppingCurrentHandles) {
  memrpc::SaBootstrapChannel channel;
  std::atomic<uint64_t> dead_session_id{0};

  channel.SetEngineDeathCallback(
      [&](uint64_t session_id) { dead_session_id.store(session_id); });
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);

  memrpc::BootstrapHandles first_handles;
  ASSERT_EQ(channel.Connect(&first_handles), memrpc::StatusCode::Ok);
  channel.SimulateEngineDeathForTest();

  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);
  memrpc::BootstrapHandles second_handles;
  ASSERT_EQ(channel.Connect(&second_handles), memrpc::StatusCode::Ok);
  ASSERT_NE(first_handles.session_id, second_handles.session_id);

  dead_session_id.store(0);
  channel.SimulateEngineDeathForTest(first_handles.session_id);
  EXPECT_EQ(dead_session_id.load(), first_handles.session_id);

  memrpc::BootstrapHandles latest_handles;
  ASSERT_EQ(channel.Connect(&latest_handles), memrpc::StatusCode::Ok);
  EXPECT_EQ(latest_handles.session_id, second_handles.session_id);

  close(first_handles.shm_fd);
  close(first_handles.high_req_event_fd);
  close(first_handles.normal_req_event_fd);
  close(first_handles.resp_event_fd);
  close(first_handles.req_credit_event_fd);
  close(first_handles.resp_credit_event_fd);
  close(second_handles.shm_fd);
  close(second_handles.high_req_event_fd);
  close(second_handles.normal_req_event_fd);
  close(second_handles.resp_event_fd);
  close(second_handles.req_credit_event_fd);
  close(second_handles.resp_credit_event_fd);
  close(latest_handles.shm_fd);
  close(latest_handles.high_req_event_fd);
  close(latest_handles.normal_req_event_fd);
  close(latest_handles.resp_event_fd);
  close(latest_handles.req_credit_event_fd);
  close(latest_handles.resp_credit_event_fd);
}

TEST(BootstrapCallbackTest, ConnectFailsWhenHandleDuplicationFails) {
  memrpc::PosixDemoBootstrapChannel channel;
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);
  channel.SetDupFailureAfterCountForTest(2);

  memrpc::BootstrapHandles handles;
  const memrpc::StatusCode status = channel.Connect(&handles);

  EXPECT_EQ(status, memrpc::StatusCode::EngineInternalError);
  EXPECT_EQ(handles.shm_fd, -1);
  EXPECT_EQ(handles.high_req_event_fd, -1);
  EXPECT_EQ(handles.normal_req_event_fd, -1);
  EXPECT_EQ(handles.resp_event_fd, -1);
  EXPECT_EQ(handles.req_credit_event_fd, -1);
  EXPECT_EQ(handles.resp_credit_event_fd, -1);
}
