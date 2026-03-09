#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "apps/minirpc/common/minirpc_codec.h"
#include "apps/minirpc/child/minirpc_service.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

TEST(MiniRpcServiceTest, EchoAndAddWork) {
  MiniRpcService service;

  EchoRequest echo_request;
  echo_request.text = "ping";
  const EchoReply echo_reply = service.Echo(echo_request);
  EXPECT_EQ(echo_reply.text, "ping");

  AddRequest add_request;
  add_request.lhs = 9;
  add_request.rhs = 4;
  const AddReply add_reply = service.Add(add_request);
  EXPECT_EQ(add_reply.sum, 13);
}

TEST(MiniRpcServiceTest, SleepWorks) {
  MiniRpcService service;

  SleepRequest sleep_request;
  sleep_request.delay_ms = 1;
  const SleepReply sleep_reply = service.Sleep(sleep_request);
  EXPECT_EQ(sleep_reply.status, 0);
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
