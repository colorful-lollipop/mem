#include <gtest/gtest.h>

#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/session.h"
#include "memrpc/client/dev_bootstrap.h"

namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) {
    close(h.shmFd);
  }
  if (h.highReqEventFd >= 0) {
    close(h.highReqEventFd);
  }
  if (h.normalReqEventFd >= 0) {
    close(h.normalReqEventFd);
  }
  if (h.respEventFd >= 0) {
    close(h.respEventFd);
  }
  if (h.reqCreditEventFd >= 0) {
    close(h.reqCreditEventFd);
  }
  if (h.respCreditEventFd >= 0) {
    close(h.respCreditEventFd);
  }
}

}  // namespace

TEST(SessionTest, AttachRejectsInvalidHeaderLayout) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles corrupt_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(corrupt_handles), MemRpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shmFd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shmFd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<MemRpc::SharedMemoryHeader*>(region);
  header->maxRequestBytes = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  MemRpc::BootstrapHandles attach_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(attach_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), MemRpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, AttachRejectsProtocolVersionMismatch) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles corrupt_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(corrupt_handles), MemRpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shmFd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shmFd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<MemRpc::SharedMemoryHeader*>(region);
  header->protocolVersion = 0;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  MemRpc::BootstrapHandles attach_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(attach_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), MemRpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, DefaultsToInlinePayloadLimits) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  ASSERT_EQ(session.Attach(handles), MemRpc::StatusCode::Ok);
  ASSERT_NE(session.Header(), nullptr);
  EXPECT_EQ(session.Header()->maxRequestBytes, MemRpc::DEFAULT_MAX_REQUEST_BYTES);
  EXPECT_EQ(session.Header()->maxResponseBytes, MemRpc::DEFAULT_MAX_RESPONSE_BYTES);
}

TEST(SessionTest, RequestRingsWrapAroundWithoutLosingCapacity) {
  MemRpc::DevBootstrapConfig config;
  config.highRingSize = 2;
  config.normalRingSize = 2;
  config.responseRingSize = 2;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);

  MemRpc::BootstrapHandles client_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::BootstrapHandles server_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(server_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, MemRpc::Session::AttachRole::Server),
            MemRpc::StatusCode::Ok);

  MemRpc::RequestRingEntry first;
  first.requestId = 1;
  MemRpc::RequestRingEntry second;
  second.requestId = 2;
  MemRpc::RequestRingEntry third;
  third.requestId = 3;

  ASSERT_EQ(client_session.PushRequest(MemRpc::QueueKind::NormalRequest, first),
            MemRpc::StatusCode::Ok);
  ASSERT_EQ(client_session.PushRequest(MemRpc::QueueKind::NormalRequest, second),
            MemRpc::StatusCode::Ok);
  EXPECT_EQ(client_session.PushRequest(MemRpc::QueueKind::NormalRequest, third),
            MemRpc::StatusCode::QueueFull);

  MemRpc::RequestRingEntry observed;
  ASSERT_TRUE(server_session.PopRequest(MemRpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 1u);

  ASSERT_EQ(client_session.PushRequest(MemRpc::QueueKind::NormalRequest, third),
            MemRpc::StatusCode::Ok);
  ASSERT_TRUE(server_session.PopRequest(MemRpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 2u);
  ASSERT_TRUE(server_session.PopRequest(MemRpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 3u);
  EXPECT_FALSE(server_session.PopRequest(MemRpc::QueueKind::NormalRequest, &observed));
}

TEST(SessionTest, ResponseRingWrapsAroundWithoutLosingCapacity) {
  MemRpc::DevBootstrapConfig config;
  config.highRingSize = 2;
  config.normalRingSize = 2;
  config.responseRingSize = 2;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);

  MemRpc::BootstrapHandles client_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::BootstrapHandles server_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(server_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, MemRpc::Session::AttachRole::Server),
            MemRpc::StatusCode::Ok);

  MemRpc::ResponseRingEntry first;
  first.requestId = 11;
  MemRpc::ResponseRingEntry second;
  second.requestId = 12;
  MemRpc::ResponseRingEntry third;
  third.requestId = 13;

  ASSERT_EQ(server_session.PushResponse(first), MemRpc::StatusCode::Ok);
  ASSERT_EQ(server_session.PushResponse(second), MemRpc::StatusCode::Ok);
  EXPECT_EQ(server_session.PushResponse(third), MemRpc::StatusCode::QueueFull);

  MemRpc::ResponseRingEntry observed;
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 11u);

  ASSERT_EQ(server_session.PushResponse(third), MemRpc::StatusCode::Ok);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 12u);
  ASSERT_TRUE(client_session.PopResponse(&observed));
  EXPECT_EQ(observed.requestId, 13u);
  EXPECT_FALSE(client_session.PopResponse(&observed));
}

TEST(SessionTest, ResponsePayloadLimitCannotExceedInlineQueueCapacity) {
  MemRpc::DevBootstrapConfig config;
  config.maxResponseBytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES + 1;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);
  MemRpc::BootstrapHandles invalid_handles = MemRpc::MakeDefaultBootstrapHandles();
  EXPECT_EQ(bootstrap->OpenSession(invalid_handles), MemRpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, RequestPayloadLimitCannotExceedInlineQueueCapacity) {
  MemRpc::DevBootstrapConfig config;
  config.maxRequestBytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES + 1;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);
  MemRpc::BootstrapHandles invalid_handles = MemRpc::MakeDefaultBootstrapHandles();
  EXPECT_EQ(bootstrap->OpenSession(invalid_handles), MemRpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, ResponsePayloadLimitCanBeSmallerThanDefault) {
  MemRpc::DevBootstrapConfig config;
  config.maxResponseBytes = MemRpc::DEFAULT_MAX_RESPONSE_BYTES - 1;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);
  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
  EXPECT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
  CloseHandles(handles);
}

TEST(SessionTest, AttachRejectsOversizedPayloadLimitsInHeader) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles corrupt_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(corrupt_handles), MemRpc::StatusCode::Ok);

  struct stat file_stat {};
  ASSERT_EQ(fstat(corrupt_handles.shmFd, &file_stat), 0);
  void* region = mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, corrupt_handles.shmFd, 0);
  ASSERT_NE(region, MAP_FAILED);
  auto* header = static_cast<MemRpc::SharedMemoryHeader*>(region);
  header->maxRequestBytes = MemRpc::DEFAULT_MAX_REQUEST_BYTES + 1;
  ASSERT_EQ(munmap(region, static_cast<size_t>(file_stat.st_size)), 0);
  CloseHandles(corrupt_handles);

  MemRpc::BootstrapHandles attach_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(attach_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  EXPECT_EQ(session.Attach(attach_handles), MemRpc::StatusCode::ProtocolMismatch);
}

TEST(SessionTest, PushRequestReturnsQueueFullWhenRingIsAtCapacity) {
  MemRpc::DevBootstrapConfig config;
  config.highRingSize = 1;
  config.normalRingSize = 1;
  config.responseRingSize = 1;

  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>(config);

  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  ASSERT_EQ(session.Attach(handles), MemRpc::StatusCode::Ok);

  MemRpc::RequestRingEntry first;
  first.requestId = 1;
  EXPECT_EQ(session.PushRequest(MemRpc::QueueKind::NormalRequest, first), MemRpc::StatusCode::Ok);

  MemRpc::RequestRingEntry second;
  second.requestId = 2;
  EXPECT_EQ(session.PushRequest(MemRpc::QueueKind::NormalRequest, second),
            MemRpc::StatusCode::QueueFull);
}

TEST(SessionTest, RejectsSecondClientAttachToSameSession) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles first_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(first_handles), MemRpc::StatusCode::Ok);
  MemRpc::BootstrapHandles second_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(second_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), MemRpc::StatusCode::InvalidArgument);
}

TEST(SessionTest, AllowsNextClientAttachAfterReset) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles first_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(first_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session first_session;
  ASSERT_EQ(first_session.Attach(first_handles), MemRpc::StatusCode::Ok);
  first_session.Reset();

  MemRpc::BootstrapHandles second_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(second_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session second_session;
  EXPECT_EQ(second_session.Attach(second_handles), MemRpc::StatusCode::Ok);
}

TEST(SessionTest, RequestEntriesExposeInlinePayloadStorage) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);

  MemRpc::Session session;
  ASSERT_EQ(session.Attach(handles), MemRpc::StatusCode::Ok);

  MemRpc::RequestRingEntry request;
  request.requestId = 55;
  request.payloadSize = 3;
  request.payload[0] = 1;
  request.payload[1] = 2;
  request.payload[2] = 3;
  ASSERT_EQ(session.PushRequest(MemRpc::QueueKind::NormalRequest, request), MemRpc::StatusCode::Ok);

  MemRpc::RequestRingEntry observed;
  ASSERT_TRUE(session.PopRequest(MemRpc::QueueKind::NormalRequest, &observed));
  EXPECT_EQ(observed.requestId, 55u);
  ASSERT_EQ(observed.payloadSize, 3u);
  EXPECT_EQ(observed.payload[0], 1u);
  EXPECT_EQ(observed.payload[1], 2u);
  EXPECT_EQ(observed.payload[2], 3u);
}

TEST(SessionTest, AttachPreservesCreditEventFds) {
  auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();

  MemRpc::BootstrapHandles client_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::BootstrapHandles server_handles = MemRpc::MakeDefaultBootstrapHandles();
  ASSERT_EQ(bootstrap->OpenSession(server_handles), MemRpc::StatusCode::Ok);

  MemRpc::Session client_session;
  ASSERT_EQ(client_session.Attach(client_handles), MemRpc::StatusCode::Ok);
  MemRpc::Session server_session;
  ASSERT_EQ(server_session.Attach(server_handles, MemRpc::Session::AttachRole::Server),
            MemRpc::StatusCode::Ok);

  EXPECT_GE(client_session.Handles().reqCreditEventFd, 0);
  EXPECT_GE(client_session.Handles().respCreditEventFd, 0);
  EXPECT_GE(server_session.Handles().reqCreditEventFd, 0);
  EXPECT_GE(server_session.Handles().respCreditEventFd, 0);
}
