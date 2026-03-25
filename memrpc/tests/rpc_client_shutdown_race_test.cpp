#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"

namespace {

constexpr MemRpc::Opcode kTestOpcode = 1u;
// ci_sweep/push_gate rerun this test with until-fail coverage, so keep the base suite cheap.
constexpr int kShutdownRaceIterations = 50;
constexpr int kShutdownRaceStressIterations = 200;
constexpr auto kMaxShutdownDuration = std::chrono::milliseconds(200);
constexpr int kConcurrentCallbackThreads = 4;
constexpr int kConcurrentCallbackInvocations = 1000;

class FailingBootstrapChannel final : public MemRpc::IBootstrapChannel {
public:
    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        handles = MemRpc::MakeDefaultBootstrapHandles();
        handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
        handles.sessionId = 1;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode CloseSession() override
    {
        return MemRpc::StatusCode::Ok;
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        callback_ = std::move(callback);
    }

    [[nodiscard]] bool HasDeathCallback() const
    {
        return static_cast<bool>(callback_);
    }

    [[nodiscard]] MemRpc::EngineDeathCallback CopyDeathCallback() const
    {
        return callback_;
    }

    void SimulateDeath(uint64_t sessionId = 0)
    {
        if (callback_) {
            callback_(sessionId);
        }
    }

private:
    MemRpc::EngineDeathCallback callback_;
};

}  // namespace

TEST(RpcClientShutdownRaceTest, InvokeAsyncFailureThenShutdownRemainsFast)
{
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

TEST(RpcClientShutdownRaceTest, InitFailureThenShutdownRemainsFast)
{
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

TEST(RpcClientShutdownRaceTest, InitFailureThenShutdownRemainsFastStress)
{
    for (int i = 0; i < kShutdownRaceStressIterations; ++i) {
        auto bootstrap = std::make_shared<FailingBootstrapChannel>();
        MemRpc::RpcClient client(bootstrap);

        EXPECT_NE(client.Init(), MemRpc::StatusCode::Ok);

        const auto start = std::chrono::steady_clock::now();
        client.Shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, kMaxShutdownDuration);
    }
}

TEST(RpcClientShutdownRaceTest, ShutdownClearsBootstrapDeathCallback)
{
    auto bootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    EXPECT_TRUE(bootstrap->HasDeathCallback());

    client.Shutdown();

    EXPECT_FALSE(bootstrap->HasDeathCallback());
    bootstrap->SimulateDeath();
}

TEST(RpcClientShutdownRaceTest, ReplacingBootstrapClearsPreviousDeathCallback)
{
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

TEST(RpcClientShutdownRaceTest, CopiedOldBootstrapDeathCallbackIsIgnoredAfterReplacement)
{
    auto firstBootstrap = std::make_shared<FailingBootstrapChannel>();
    auto secondBootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcClient client(firstBootstrap);

    const auto staleCallback = firstBootstrap->CopyDeathCallback();
    ASSERT_TRUE(static_cast<bool>(staleCallback));

    client.SetBootstrapChannel(secondBootstrap);
    const auto before = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(before.lifecycleState, MemRpc::ClientLifecycleState::Uninitialized);

    staleCallback(0);

    const auto after = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(after.lifecycleState, before.lifecycleState);
    EXPECT_EQ(after.lastTrigger, before.lastTrigger);
    EXPECT_EQ(after.currentSessionId, before.currentSessionId);

    client.Shutdown();
}

TEST(RpcClientShutdownRaceTest, CopiedBootstrapDeathCallbackIsIgnoredAfterShutdown)
{
    auto bootstrap = std::make_shared<FailingBootstrapChannel>();
    MemRpc::RpcClient client(bootstrap);

    const auto copiedCallback = bootstrap->CopyDeathCallback();
    ASSERT_TRUE(static_cast<bool>(copiedCallback));

    client.Shutdown();
    const auto before = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(before.lifecycleState, MemRpc::ClientLifecycleState::Closed);

    copiedCallback(0);

    const auto after = client.GetRecoveryRuntimeSnapshot();
    EXPECT_EQ(after.lifecycleState, before.lifecycleState);
    EXPECT_EQ(after.lastTrigger, before.lastTrigger);
    EXPECT_EQ(after.currentSessionId, before.currentSessionId);
}

TEST(RpcClientShutdownRaceTest, ConcurrentStaleBootstrapDeathCallbacksAreIgnoredAfterReplacement)
{
    for (int iteration = 0; iteration < kShutdownRaceIterations; ++iteration) {
        auto firstBootstrap = std::make_shared<FailingBootstrapChannel>();
        auto secondBootstrap = std::make_shared<FailingBootstrapChannel>();
        MemRpc::RpcClient client(firstBootstrap);

        const auto staleCallback = firstBootstrap->CopyDeathCallback();
        ASSERT_TRUE(static_cast<bool>(staleCallback));

        client.SetBootstrapChannel(secondBootstrap);
        const auto before = client.GetRecoveryRuntimeSnapshot();
        EXPECT_EQ(before.lifecycleState, MemRpc::ClientLifecycleState::Uninitialized);

        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(kConcurrentCallbackThreads);
        for (int worker = 0; worker < kConcurrentCallbackThreads; ++worker) {
            workers.emplace_back([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int invoke = 0; invoke < kConcurrentCallbackInvocations; ++invoke) {
                    staleCallback(0);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        const auto after = client.GetRecoveryRuntimeSnapshot();
        EXPECT_EQ(after.lifecycleState, before.lifecycleState);
        EXPECT_EQ(after.lastTrigger, before.lastTrigger);
        EXPECT_EQ(after.currentSessionId, before.currentSessionId);

        client.Shutdown();
    }
}

TEST(RpcClientShutdownRaceTest, ConcurrentCopiedBootstrapDeathCallbacksAreIgnoredAfterShutdown)
{
    for (int iteration = 0; iteration < kShutdownRaceIterations; ++iteration) {
        auto bootstrap = std::make_shared<FailingBootstrapChannel>();
        MemRpc::RpcClient client(bootstrap);

        const auto copiedCallback = bootstrap->CopyDeathCallback();
        ASSERT_TRUE(static_cast<bool>(copiedCallback));

        client.Shutdown();
        const auto before = client.GetRecoveryRuntimeSnapshot();
        EXPECT_EQ(before.lifecycleState, MemRpc::ClientLifecycleState::Closed);

        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(kConcurrentCallbackThreads);
        for (int worker = 0; worker < kConcurrentCallbackThreads; ++worker) {
            workers.emplace_back([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int invoke = 0; invoke < kConcurrentCallbackInvocations; ++invoke) {
                    copiedCallback(0);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        const auto after = client.GetRecoveryRuntimeSnapshot();
        EXPECT_EQ(after.lifecycleState, before.lifecycleState);
        EXPECT_EQ(after.lastTrigger, before.lastTrigger);
        EXPECT_EQ(after.currentSessionId, before.currentSessionId);
    }
}

TEST(RpcClientShutdownRaceTest, InvokeAsyncWaitFailureThenShutdownRemainsFast)
{
    for (int i = 0; i < kShutdownRaceIterations; ++i) {
        auto bootstrap = std::make_shared<FailingBootstrapChannel>();
        MemRpc::RpcClient client(bootstrap);

        MemRpc::RpcCall call;
        call.opcode = kTestOpcode;

        MemRpc::RpcReply reply;
        EXPECT_NE(client.InvokeAsync(call).Wait(&reply), MemRpc::StatusCode::Ok);

        const auto start = std::chrono::steady_clock::now();
        client.Shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, kMaxShutdownDuration);
    }
}
