#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/bootstrap.h"
#include "memrpc/rpc_client.h"
#include "memrpc/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode StartEngine() override {
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode Connect(MemRpc::BootstrapHandles* handles) override {
    if (handles != nullptr) {
      handles->protocol_version = 1;
    }
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode NotifyPeerRestarted() override {
    return MemRpc::StatusCode::Ok;
  }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

 private:
  MemRpc::EngineDeathCallback callback_;
};

}  // namespace

TEST(ApiHeadersTest, PublicHeadersCompose) {
  MemRpc::RpcClient client;
  MemRpc::RpcServer server;
  FakeBootstrapChannel bootstrap;
  MemRpc::BootstrapHandles handles;

  EXPECT_EQ(bootstrap.StartEngine(), MemRpc::StatusCode::Ok);
  EXPECT_EQ(bootstrap.Connect(&handles), MemRpc::StatusCode::Ok);
  EXPECT_EQ(handles.protocol_version, 1u);
  EXPECT_EQ(bootstrap.NotifyPeerRestarted(), MemRpc::StatusCode::Ok);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
}
