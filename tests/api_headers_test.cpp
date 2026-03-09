#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "memrpc/bootstrap.h"
#include "memrpc/client.h"
#include "memrpc/handler.h"
#include "memrpc/server.h"

namespace {

class FakeBootstrapChannel : public memrpc::IBootstrapChannel {
 public:
  memrpc::StatusCode StartEngine() override {
    return memrpc::StatusCode::Ok;
  }

  memrpc::StatusCode Connect(memrpc::BootstrapHandles* handles) override {
    if (handles != nullptr) {
      handles->protocol_version = 1;
    }
    return memrpc::StatusCode::Ok;
  }

  memrpc::StatusCode NotifyPeerRestarted() override {
    return memrpc::StatusCode::Ok;
  }

  void SetEngineDeathCallback(memrpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

 private:
  memrpc::EngineDeathCallback callback_;
};

class FakeHandler : public memrpc::IScanHandler {
 public:
  memrpc::ScanResult HandleScan(const memrpc::ScanRequest& request) override {
    memrpc::ScanResult result;
    result.message = request.file_path;
    return result;
  }
};

}  // namespace

TEST(ApiHeadersTest, PublicHeadersCompose) {
  memrpc::EngineClient client;
  memrpc::EngineServer server;
  FakeBootstrapChannel bootstrap;
  FakeHandler handler;
  memrpc::BootstrapHandles handles;

  EXPECT_EQ(bootstrap.StartEngine(), memrpc::StatusCode::Ok);
  EXPECT_EQ(bootstrap.Connect(&handles), memrpc::StatusCode::Ok);
  EXPECT_EQ(handles.protocol_version, 1u);
  EXPECT_EQ(bootstrap.NotifyPeerRestarted(), memrpc::StatusCode::Ok);

  memrpc::ScanRequest request;
  request.file_path = "/tmp/file";
  const memrpc::ScanResult result = handler.HandleScan(request);
  EXPECT_EQ(result.message, request.file_path);
  EXPECT_FALSE(std::is_copy_constructible_v<memrpc::EngineClient>);
  EXPECT_FALSE(std::is_copy_constructible_v<memrpc::EngineServer>);
}
