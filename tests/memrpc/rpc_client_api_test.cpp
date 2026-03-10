#include <gtest/gtest.h>

#include <type_traits>
#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode StartEngine() override { return MemRpc::StatusCode::Ok; }

  MemRpc::StatusCode Connect(MemRpc::BootstrapHandles* handles) override {
    if (handles != nullptr) {
      handles->protocol_version = 1;
    }
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode NotifyPeerRestarted() override { return MemRpc::StatusCode::Ok; }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

 private:
  MemRpc::EngineDeathCallback callback_;
};

}  // namespace

TEST(RpcClientApiTest, PublicHeaderComposes) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);
  client.SetEventCallback([](const MemRpc::RpcEvent& event) {
    EXPECT_GE(event.event_domain, 0u);
    EXPECT_GE(event.event_type, 0u);
  });

  MemRpc::RpcCall call;
  call.opcode = MemRpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{1, 2, 3};

  MemRpc::RpcReply reply;
  MemRpc::RpcClientRuntimeStats stats;
  EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
  EXPECT_EQ(stats.pending_calls, 0u);
  EXPECT_EQ(call.priority, MemRpc::Priority::Normal);
  EXPECT_EQ(call.admission_timeout_ms, 1000u);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);

  auto future = client.InvokeAsync(call);
  EXPECT_TRUE(future.IsReady());
  EXPECT_NE(future.Wait(&reply), MemRpc::StatusCode::Ok);
}

TEST(RpcClientApiTest, PublicHeaderSupportsMoveAwareCallAndReplyApis) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  MemRpc::RpcCall call;
  call.opcode = MemRpc::Opcode::ScanFile;
  call.payload = std::vector<uint8_t>{4, 5, 6};

  MemRpc::RpcReply reply;
  auto future = client.InvokeAsync(std::move(call));
  EXPECT_TRUE(future.IsReady());
  EXPECT_NE(future.WaitAndTake(&reply), MemRpc::StatusCode::Ok);
}

TEST(RpcClientApiTest, AdmissionTimeoutCanBeConfiguredIndependently) {
  MemRpc::RpcCall call;
  call.admission_timeout_ms = 0;
  call.queue_timeout_ms = 250;
  call.exec_timeout_ms = 500;

  EXPECT_EQ(call.admission_timeout_ms, 0u);
  EXPECT_EQ(call.queue_timeout_ms, 250u);
  EXPECT_EQ(call.exec_timeout_ms, 500u);
}

TEST(RpcClientApiTest, BootstrapHandlesExposeCreditEventFds) {
  MemRpc::BootstrapHandles handles;

  EXPECT_EQ(handles.req_credit_event_fd, -1);
  EXPECT_EQ(handles.resp_credit_event_fd, -1);
}
