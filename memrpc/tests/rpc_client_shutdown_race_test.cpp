#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"

namespace {

constexpr MemRpc::Opcode kTestOpcode = 1u;
// ci_sweep/push_gate rerun this test with until-fail coverage, so keep the base suite cheap.
constexpr int kShutdownRaceIterations = 50;
constexpr auto kMaxShutdownDuration = std::chrono::milliseconds(200);

class FailingBootstrapChannel final : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override {
    handles = MemRpc::MakeDefaultBootstrapHandles();
    handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
    handles.sessionId = 1;
    return MemRpc::StatusCode::Ok;
  }

  MemRpc::StatusCode CloseSession() override {
    return MemRpc::StatusCode::Ok;
  }

  void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override {
    callback_ = std::move(callback);
  }

  [[nodiscard]] bool HasDeathCallback() const {
    return static_cast<bool>(callback_);
  }

  void SimulateDeath(uint64_t sessionId = 0) {
    if (callback_) {
      callback_(sessionId);
    }
  }

 private:
  MemRpc::EngineDeathCallback callback_;
};

}  // namespace

TEST(RpcClientShutdownRaceTest, InvokeAsyncFailureThenShutdownRemainsFast) {
  for (int i = 0; i < kShutdownRaceIterations; ++i) {
    auto bootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;

    MemRpc::RpcFuture future = client.InvokeAsync(call);
    ASSERT_TRUE(future.IsReady());

    MemRpc::RpcReply reply;
    EXPECT_NE(future.Wait(&reply), MemRpc::StatusCode::Ok);

    const auto start = std::chrono::steady_clock::now();
    client.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, kMaxShutdownDuration);
  }
}

TEST(RpcClientShutdownRaceTest, InitFailureThenShutdownRemainsFast) {
  for (int i = 0; i < kShutdownRaceIterations; ++i) {
    auto bootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    EXPECT_NE(client.Init(), MemRpc::StatusCode::Ok);

    const auto start = std::chrono::steady_clock::now();
    client.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, kMaxShutdownDuration);
  }
}

TEST(RpcClientShutdownRaceTest, ShutdownClearsBootstrapDeathCallback) {
  auto bootstrap = std::make_shared<FailingBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  EXPECT_TRUE(bootstrap->HasDeathCallback());

  client.Shutdown();

  EXPECT_FALSE(bootstrap->HasDeathCallback());
  bootstrap->SimulateDeath();
}

TEST(RpcClientShutdownRaceTest, ReplacingBootstrapClearsPreviousDeathCallback) {
  auto firstBootstrap = std::make_shared<FailingBootstrapChannel>();
  auto secondBootstrap = std::make_shared<FailingBootstrapChannel>();
  MemRpc::RpcClient client(firstBootstrap);

  EXPECT_TRUE(firstBootstrap->HasDeathCallback());

  client.SetBootstrapChannel(secondBootstrap);

  EXPECT_FALSE(firstBootstrap->HasDeathCallback());
  EXPECT_TRUE(secondBootstrap->HasDeathCallback());

  client.Shutdown();
  EXPECT_FALSE(secondBootstrap->HasDeathCallback());
}

TEST(RpcClientShutdownRaceTest, SyncInvokeFailureThenShutdownRemainsFast) {
  for (int i = 0; i < kShutdownRaceIterations; ++i) {
    auto bootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcSyncClient client(bootstrap);

    MemRpc::RpcCall call;
    call.opcode = kTestOpcode;

    MemRpc::RpcReply reply;
    EXPECT_NE(client.InvokeSync(call, &reply), MemRpc::StatusCode::Ok);

    const auto start = std::chrono::steady_clock::now();
    client.Shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, kMaxShutdownDuration);
  }
}
