#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"
#include "ves/ves_session_service.h"

namespace VirusExecutorService {

namespace {

constexpr MemRpc::Opcode kBlockingOpcode = static_cast<MemRpc::Opcode>(901);

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

class SessionServiceBootstrapChannel final : public MemRpc::IBootstrapChannel {
public:
    explicit SessionServiceBootstrapChannel(std::shared_ptr<EngineSessionService> sessionService)
        : sessionService_(std::move(sessionService))
    {
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        return sessionService_ != nullptr ? sessionService_->OpenSession(handles) : MemRpc::StatusCode::InvalidArgument;
    }

    MemRpc::StatusCode CloseSession() override
    {
        return sessionService_ != nullptr ? sessionService_->CloseSession() : MemRpc::StatusCode::Ok;
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        (void)callback;
    }

private:
    std::shared_ptr<EngineSessionService> sessionService_;
};

class BlockingRegistrar final : public RpcHandlerRegistrar {
public:
    void RegisterHandlers(RpcHandlerSink* sink) override
    {
        ASSERT_NE(sink, nullptr);
        sink->RegisterHandler(kBlockingOpcode, [this](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply* reply) {
            ASSERT_NE(reply, nullptr);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = true;
            }
            cv_.notify_all();

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return allowReply_; });
            reply->status = MemRpc::StatusCode::Ok;
        });
    }

    bool WaitUntilRunning(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return running_; });
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            allowReply_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool allowReply_ = false;
};

}  // namespace

TEST(VesHealthTest, SessionServiceRuntimeStatsStayIdleWithoutRequests)
{
    BlockingRegistrar registrar;
    auto sessionService = std::make_shared<EngineSessionService>(std::vector<RpcHandlerRegistrar*>{&registrar});
    auto bootstrap = std::make_shared<SessionServiceBootstrapChannel>(sessionService);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    const auto stats = sessionService->GetRuntimeStats();
    EXPECT_EQ(stats.activeRequestExecutions, 0u);
    EXPECT_EQ(stats.oldestExecutionAgeMs, 0u);

    client.Shutdown();
}

TEST(VesHealthTest, SessionServiceRuntimeStatsTrackActiveRequests)
{
    BlockingRegistrar registrar;
    auto sessionService = std::make_shared<EngineSessionService>(std::vector<RpcHandlerRegistrar*>{&registrar});
    auto bootstrap = std::make_shared<SessionServiceBootstrapChannel>(sessionService);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kBlockingOpcode;
    auto future = client.InvokeAsync(call);

    ASSERT_TRUE(registrar.WaitUntilRunning(std::chrono::milliseconds(200)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const auto busyStats = sessionService->GetRuntimeStats();
    EXPECT_EQ(busyStats.activeRequestExecutions, 1u);
    EXPECT_GE(busyStats.oldestExecutionAgeMs, 1u);

    registrar.Release();

    MemRpc::RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::Ok);

    ASSERT_TRUE(WaitFor(
        [&]() {
            const auto stats = sessionService->GetRuntimeStats();
            return stats.activeRequestExecutions == 0 && stats.oldestExecutionAgeMs == 0;
        },
        std::chrono::milliseconds(200)));

    client.Shutdown();
}

}  // namespace VirusExecutorService
