#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace {

class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    handles = MemRpc::MakeDefaultBootstrapHandles();
    handles.protocolVersion = 1;
    handles.sessionId = 1;
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
  MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

  EXPECT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);
  EXPECT_EQ(handles.protocolVersion, 1U);
  EXPECT_EQ(bootstrap.CloseSession(), MemRpc::StatusCode::Ok);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<MemRpc::RpcServer>);
}
