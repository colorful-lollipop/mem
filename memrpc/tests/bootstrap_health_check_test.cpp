#include <gtest/gtest.h>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/core/bootstrap.h"

namespace {

class FakeBootstrapChannel final : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    handles = {};
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

TEST(BootstrapHealthCheckTest, InterfaceDefaultsToUnsupported) {
  FakeBootstrapChannel bootstrap;
  const auto result = bootstrap.CheckHealth(123);
  EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Unsupported);
  EXPECT_EQ(result.sessionId, 0u);
}

TEST(BootstrapHealthCheckTest, DevBootstrapReportsUnsupported) {
  MemRpc::DevBootstrapChannel bootstrap;
  const auto result = bootstrap.CheckHealth(456);
  EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Unsupported);
  EXPECT_EQ(result.sessionId, 0u);
}

TEST(BootstrapHealthCheckTest, SaBootstrapReportsUnsupported) {
  MemRpc::SaBootstrapChannel bootstrap;
  const auto result = bootstrap.CheckHealth(789);
  EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Unsupported);
  EXPECT_EQ(result.sessionId, 0u);
}
