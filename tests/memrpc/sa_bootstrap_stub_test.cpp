#include <gtest/gtest.h>

#include <unistd.h>

#include "core/protocol.h"
#include "memrpc/client/sa_bootstrap.h"

TEST(SaBootstrapStubTest, DefaultConfigIsStable) {
  const memrpc::SaBootstrapConfig config;
  EXPECT_TRUE(config.service_name.empty());
  EXPECT_TRUE(config.instance_name.empty());
  EXPECT_FALSE(config.lazy_connect);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapConnectsViaOpenSession) {
  memrpc::SaBootstrapChannel channel;
  memrpc::BootstrapHandles handles;

  EXPECT_EQ(channel.OpenSession(&handles), memrpc::StatusCode::Ok);
  EXPECT_GE(handles.shm_fd, 0);
  EXPECT_GE(handles.high_req_event_fd, 0);
  EXPECT_GE(handles.normal_req_event_fd, 0);
  EXPECT_GE(handles.resp_event_fd, 0);
  EXPECT_GE(handles.req_credit_event_fd, 0);
  EXPECT_GE(handles.resp_credit_event_fd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::PROTOCOL_VERSION);
  EXPECT_EQ(channel.CloseSession(), memrpc::StatusCode::Ok);

  close(handles.shm_fd);
  close(handles.high_req_event_fd);
  close(handles.normal_req_event_fd);
  close(handles.resp_event_fd);
  close(handles.req_credit_event_fd);
  close(handles.resp_credit_event_fd);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapExposesServerHandlesForForkedEngine) {
  memrpc::SaBootstrapChannel channel;
  memrpc::BootstrapHandles unused;
  ASSERT_EQ(channel.OpenSession(&unused), memrpc::StatusCode::Ok);
  close(unused.shm_fd);
  close(unused.high_req_event_fd);
  close(unused.normal_req_event_fd);
  close(unused.resp_event_fd);
  close(unused.req_credit_event_fd);
  close(unused.resp_credit_event_fd);

  const memrpc::BootstrapHandles handles = channel.server_handles();
  EXPECT_GE(handles.shm_fd, 0);
  EXPECT_GE(handles.high_req_event_fd, 0);
  EXPECT_GE(handles.normal_req_event_fd, 0);
  EXPECT_GE(handles.resp_event_fd, 0);
  EXPECT_GE(handles.req_credit_event_fd, 0);
  EXPECT_GE(handles.resp_credit_event_fd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::PROTOCOL_VERSION);

  close(handles.shm_fd);
  close(handles.high_req_event_fd);
  close(handles.normal_req_event_fd);
  close(handles.resp_event_fd);
  close(handles.req_credit_event_fd);
  close(handles.resp_credit_event_fd);
}
