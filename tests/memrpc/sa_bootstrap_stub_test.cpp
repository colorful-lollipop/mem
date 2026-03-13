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

  EXPECT_EQ(channel.OpenSession(handles), memrpc::StatusCode::Ok);
  EXPECT_GE(handles.shmFd, 0);
  EXPECT_GE(handles.highReqEventFd, 0);
  EXPECT_GE(handles.normalReqEventFd, 0);
  EXPECT_GE(handles.respEventFd, 0);
  EXPECT_GE(handles.reqCreditEventFd, 0);
  EXPECT_GE(handles.respCreditEventFd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::PROTOCOL_VERSION);
  EXPECT_EQ(channel.CloseSession(), memrpc::StatusCode::Ok);

  close(handles.shmFd);
  close(handles.highReqEventFd);
  close(handles.normalReqEventFd);
  close(handles.respEventFd);
  close(handles.reqCreditEventFd);
  close(handles.respCreditEventFd);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapExposesServerHandlesForForkedEngine) {
  memrpc::SaBootstrapChannel channel;
  memrpc::BootstrapHandles unused;
  ASSERT_EQ(channel.OpenSession(unused), memrpc::StatusCode::Ok);
  close(unused.shmFd);
  close(unused.highReqEventFd);
  close(unused.normalReqEventFd);
  close(unused.respEventFd);
  close(unused.reqCreditEventFd);
  close(unused.respCreditEventFd);

  const memrpc::BootstrapHandles handles = channel.server_handles();
  EXPECT_GE(handles.shmFd, 0);
  EXPECT_GE(handles.highReqEventFd, 0);
  EXPECT_GE(handles.normalReqEventFd, 0);
  EXPECT_GE(handles.respEventFd, 0);
  EXPECT_GE(handles.reqCreditEventFd, 0);
  EXPECT_GE(handles.respCreditEventFd, 0);
  EXPECT_EQ(handles.protocol_version, memrpc::PROTOCOL_VERSION);

  close(handles.shmFd);
  close(handles.highReqEventFd);
  close(handles.normalReqEventFd);
  close(handles.respEventFd);
  close(handles.reqCreditEventFd);
  close(handles.respCreditEventFd);
}
