#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace MemRpc = OHOS::Security::VirusProtectionService::MemRpc;

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles* handles) override {
    if (handles != nullptr) {
      *handles = MemRpc::BootstrapHandles{};
      handles->protocol_version = 1;
      handles->session_id = 1;
    }
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode CloseSession() override {
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

  EXPECT_EQ(bootstrap.OpenSession(&handles), MemRpc::StatusCode::Ok);
  EXPECT_EQ(handles.protocol_version, 1u);
  EXPECT_EQ(bootstrap.CloseSession(), MemRpc::StatusCode::Ok);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
}
