#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/protocol.h"
#include "memrpc/client/sa_bootstrap.h"

TEST(SaBootstrapStubTest, DefaultConfigIsStable) {
  const MemRpc::SaBootstrapConfig config;
  EXPECT_TRUE(config.serviceName.empty());
  EXPECT_TRUE(config.instanceName.empty());
  EXPECT_FALSE(config.lazyConnect);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapConnectsViaOpenSession) {
  MemRpc::SaBootstrapChannel channel;
  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

  EXPECT_EQ(channel.OpenSession(handles), MemRpc::StatusCode::Ok);
  EXPECT_GE(handles.shmFd, 0);
  EXPECT_GE(handles.highReqEventFd, 0);
  EXPECT_GE(handles.normalReqEventFd, 0);
  EXPECT_GE(handles.respEventFd, 0);
  EXPECT_GE(handles.reqCreditEventFd, 0);
  EXPECT_GE(handles.respCreditEventFd, 0);
  EXPECT_EQ(handles.protocolVersion, MemRpc::PROTOCOL_VERSION);
  EXPECT_EQ(channel.CloseSession(), MemRpc::StatusCode::Ok);

  close(handles.shmFd);
  close(handles.highReqEventFd);
  close(handles.normalReqEventFd);
  close(handles.respEventFd);
  close(handles.reqCreditEventFd);
  close(handles.respCreditEventFd);
}

TEST(SaBootstrapStubTest, FakeSaBootstrapExposesServerHandlesForForkedEngine) {
  MemRpc::SaBootstrapChannel channel;
  MemRpc::BootstrapHandles unused = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(channel.OpenSession(unused), MemRpc::StatusCode::Ok);
  close(unused.shmFd);
  close(unused.highReqEventFd);
  close(unused.normalReqEventFd);
  close(unused.respEventFd);
  close(unused.reqCreditEventFd);
  close(unused.respCreditEventFd);

  const MemRpc::BootstrapHandles handles = channel.ServerHandles();
  EXPECT_GE(handles.shmFd, 0);
  EXPECT_GE(handles.highReqEventFd, 0);
  EXPECT_GE(handles.normalReqEventFd, 0);
  EXPECT_GE(handles.respEventFd, 0);
  EXPECT_GE(handles.reqCreditEventFd, 0);
  EXPECT_GE(handles.respCreditEventFd, 0);
  EXPECT_EQ(handles.protocolVersion, MemRpc::PROTOCOL_VERSION);

  close(handles.shmFd);
  close(handles.highReqEventFd);
  close(handles.normalReqEventFd);
  close(handles.respEventFd);
  close(handles.reqCreditEventFd);
  close(handles.respCreditEventFd);
}
