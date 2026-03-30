#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kEchoOpcode = static_cast<MemRpc::Opcode>(210);
constexpr MemRpc::Opcode kBlockingOpcode = static_cast<MemRpc::Opcode>(211);
constexpr auto kWaitTimeout = std::chrono::milliseconds(1000);
constexpr auto kRecoveryDelay = std::chrono::milliseconds(150);
constexpr int kRecoveryCycles = 4;

void CloseHandles(MemRpc::BootstrapHandles& handles)
{
    if (handles.shmFd >= 0) {
        close(handles.shmFd);
    }
    if (handles.highReqEventFd >= 0) {
        close(handles.highReqEventFd);
    }
    if (handles.normalReqEventFd >= 0) {
        close(handles.normalReqEventFd);
    }
    if (handles.respEventFd >= 0) {
        close(handles.respEventFd);
    }
    if (handles.reqCreditEventFd >= 0) {
        close(handles.reqCreditEventFd);
    }
    if (handles.respCreditEventFd >= 0) {
        close(handles.respCreditEventFd);
    }
}

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

int CountOpenFileDescriptors()
{
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }

    const int dirFd = dirfd(dir);
    int count = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry == nullptr) {
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (std::atoi(entry->d_name) == dirFd) {
            continue;
        }
        ++count;
    }
    closedir(dir);
    return count;
}

void PrewarmSessionHost(const std::shared_ptr<MemRpc::DevBootstrapChannel>& bootstrap)
{
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
}

void StartServer(const std::shared_ptr<MemRpc::DevBootstrapChannel>& bootstrap,
                 MemRpc::RpcServer* server,
                 const std::function<void(const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*)>& echoHandler,
                 const std::function<void(const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*)>& blockingHandler)
{
    server->SetBootstrapHandles(bootstrap->serverHandles());
    server->RegisterHandler(kEchoOpcode, echoHandler);
    server->RegisterHandler(kBlockingOpcode, blockingHandler);
    ASSERT_EQ(server->Start(), MemRpc::StatusCode::Ok);
}

class CountingBootstrapChannel final : public MemRpc::IBootstrapChannel {
public:
    explicit CountingBootstrapChannel(std::shared_ptr<MemRpc::DevBootstrapChannel> delegate)
        : delegate_(std::move(delegate))
    {
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        openCount_.fetch_add(1, std::memory_order_relaxed);
        return delegate_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        closeCount_.fetch_add(1, std::memory_order_relaxed);
        return delegate_->CloseSession();
    }

    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override
    {
        return delegate_->CheckHealth(expectedSessionId);
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        delegate_->SetEngineDeathCallback(std::move(callback));
    }

    [[nodiscard]] int openCount() const
    {
        return openCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int closeCount() const
    {
        return closeCount_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<MemRpc::DevBootstrapChannel> delegate_;
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
};

}  // namespace

namespace MemRpc {

TEST(RpcClientCrashRecoveryTest, EngineCrashFailsInFlightRequestAndClosesLiveSession)
{
    auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
    PrewarmSessionHost(rawBootstrap);

    std::atomic<bool> blockingHandlerEntered{false};
    std::atomic<bool> allowBlockingHandlerExit{false};

    RpcServer server;
    StartServer(rawBootstrap,
                &server,
                [](const RpcServerCall&, RpcServerReply* reply) {
                    reply->status = StatusCode::Ok;
                },
                [&](const RpcServerCall&, RpcServerReply* reply) {
                    blockingHandlerEntered.store(true, std::memory_order_release);
                    while (!allowBlockingHandlerExit.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    reply->status = StatusCode::Ok;
                });

    auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
    RpcClient client(bootstrap);
    RecoveryPolicy policy;
    policy.onEngineDeath = [](const EngineDeathReport&) {
        return RecoveryDecision{RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    const uint64_t liveSessionId = client.GetRecoveryRuntimeSnapshot().currentSessionId;
    ASSERT_NE(liveSessionId, 0u);

    RpcCall call;
    call.opcode = kBlockingOpcode;
    auto future = client.InvokeAsync(call);

    ASSERT_TRUE(WaitFor([&]() { return blockingHandlerEntered.load(std::memory_order_acquire); }, kWaitTimeout));

    rawBootstrap->SimulateEngineDeathForTest(liveSessionId);

    RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::CrashedDuringExecution);

    ASSERT_TRUE(WaitFor(
        [&]() {
            const auto snapshot = client.GetRecoveryRuntimeSnapshot();
            return snapshot.lifecycleState == ClientLifecycleState::NoSession && snapshot.currentSessionId == 0u;
        },
        kWaitTimeout));

    EXPECT_EQ(bootstrap->closeCount(), 1);

    allowBlockingHandlerExit.store(true, std::memory_order_release);
    server.Stop();
    client.Shutdown();
}

TEST(RpcClientCrashRecoveryTest, RepeatedCrashRecoveryReopensNewSessionsWithoutFdGrowth)
{
    auto rawBootstrap = std::make_shared<DevBootstrapChannel>();
    PrewarmSessionHost(rawBootstrap);

    auto echoHandler = [](const RpcServerCall&, RpcServerReply* reply) {
        reply->status = StatusCode::Ok;
    };
    auto blockingHandler = [](const RpcServerCall&, RpcServerReply* reply) {
        reply->status = StatusCode::Ok;
    };

    auto server = std::make_unique<RpcServer>();
    StartServer(rawBootstrap, server.get(), echoHandler, blockingHandler);

    auto bootstrap = std::make_shared<CountingBootstrapChannel>(rawBootstrap);
    RpcClient client(bootstrap);
    RecoveryPolicy policy;
    policy.onEngineDeath = [](const EngineDeathReport&) {
        return RecoveryDecision{RecoveryAction::Restart, static_cast<uint32_t>(kRecoveryDelay.count())};
    };
    client.SetRecoveryPolicy(std::move(policy));
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall echoCall;
    echoCall.opcode = kEchoOpcode;
    RpcReply initialReply;
    ASSERT_EQ(client.InvokeAsync(echoCall).Wait(&initialReply), StatusCode::Ok);

    ASSERT_TRUE(WaitFor(
        [&]() { return client.GetRecoveryRuntimeSnapshot().lifecycleState == ClientLifecycleState::Active; },
        kWaitTimeout));
    const int baselineFdCount = CountOpenFileDescriptors();
    ASSERT_GE(baselineFdCount, 0);

    uint64_t previousSessionId = client.GetRecoveryRuntimeSnapshot().currentSessionId;
    ASSERT_NE(previousSessionId, 0u);

    for (int cycle = 0; cycle < kRecoveryCycles; ++cycle) {
        rawBootstrap->SimulateEngineDeathForTest(previousSessionId);
        server->Stop();

        auto restartedServer = std::make_unique<RpcServer>();
        std::thread reopenThread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            PrewarmSessionHost(rawBootstrap);
            StartServer(rawBootstrap, restartedServer.get(), echoHandler, blockingHandler);
        });

        RpcReply recoveredReply;
        EXPECT_EQ(client.InvokeAsync(echoCall).Wait(&recoveredReply), StatusCode::Ok) << "cycle " << cycle;
        reopenThread.join();

        ASSERT_TRUE(WaitFor(
            [&]() {
                const auto snapshot = client.GetRecoveryRuntimeSnapshot();
                return snapshot.lifecycleState == ClientLifecycleState::Active &&
                       snapshot.currentSessionId != 0u && snapshot.currentSessionId != previousSessionId;
            },
            kWaitTimeout))
            << "cycle " << cycle;

        const auto snapshot = client.GetRecoveryRuntimeSnapshot();
        EXPECT_EQ(bootstrap->closeCount(), cycle + 1);
        EXPECT_EQ(bootstrap->openCount(), cycle + 2);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int steadyFdCount = CountOpenFileDescriptors();
        ASSERT_GE(steadyFdCount, 0);
        EXPECT_LE(steadyFdCount, baselineFdCount + 1) << "cycle " << cycle;

        previousSessionId = snapshot.currentSessionId;
        server = std::move(restartedServer);
    }

    client.Shutdown();
    server->Stop();
}

}  // namespace MemRpc
