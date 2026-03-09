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

TEST(SaBootstrapStubTest, FakeSaBootstrapConnectsAfterStartEngine) {
  memrpc::SaBootstrapChannel channel;
  memrpc::BootstrapHandles handles;

  EXPECT_EQ(channel.Connect(&handles), memrpc::StatusCode::InvalidArgument);
  EXPECT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);
  EXPECT_EQ(channel.Connect(&handles), memrpc::StatusCode::Ok);
  EXPECT_GE(handles.shm_fd, 0);
  EXPECT_GE(handles.high_req_event_fd, 0);
  EXPECT_GE(handles.normal_req_event_fd, 0);
  EXPECT_GE(handles.resp_event_fd, 0);
  EXPECT_GE(handles.req_credit_event_fd, 0);
  EXPECT_GE(handles.resp_credit_event_fd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::kProtocolVersion);
  EXPECT_EQ(channel.NotifyPeerRestarted(), memrpc::StatusCode::Ok);

  close(handles.shm_fd);
  close(handles.high_req_event_fd);
  close(handles.normal_req_event_fd);
  close(handles.resp_event_fd);
  close(handles.req_credit_event_fd);
  close(handles.resp_credit_event_fd);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapExposesServerHandlesForForkedEngine) {
  memrpc::SaBootstrapChannel channel;
  ASSERT_EQ(channel.StartEngine(), memrpc::StatusCode::Ok);

  const memrpc::BootstrapHandles handles = channel.server_handles();
  EXPECT_GE(handles.shm_fd, 0);
  EXPECT_GE(handles.high_req_event_fd, 0);
  EXPECT_GE(handles.normal_req_event_fd, 0);
  EXPECT_GE(handles.resp_event_fd, 0);
  EXPECT_GE(handles.req_credit_event_fd, 0);
  EXPECT_GE(handles.resp_credit_event_fd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::kProtocolVersion);

  close(handles.shm_fd);
  close(handles.high_req_event_fd);
  close(handles.normal_req_event_fd);
  close(handles.resp_event_fd);
  close(handles.req_credit_event_fd);
  close(handles.resp_credit_event_fd);
}
