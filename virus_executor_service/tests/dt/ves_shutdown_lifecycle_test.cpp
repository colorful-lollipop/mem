#include <gtest/gtest.h>

#include <cstdlib>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "service/rpc_handler_registrar.h"
#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"
#include "transport/ves_control_proxy.h"
#include "ves/ves_engine_service.h"
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

MemRpc::RpcFuture StartAsyncScan(MemRpc::RpcClient* client, const std::string& path)
{
    EXPECT_NE(client, nullptr);
    ScanTask task;
    task.path = path;

    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(VesOpcode::ScanFile);
    EXPECT_TRUE(MemRpc::EncodeMessage(task, &call.payload));
    return client->InvokeAsync(std::move(call));
}

VesBootstrapChannel::ControlLoader MakeStaticControlLoader(const OHOS::sptr<IVirusProtectionExecutor>& control)
{
    return [control]() -> OHOS::sptr<IVirusProtectionExecutor> { return control; };
}

void CloseHandles(MemRpc::BootstrapHandles* handles)
{
    if (handles == nullptr) {
        return;
    }

    int* fds[] = {
        &handles->shmFd,
        &handles->highReqEventFd,
        &handles->normalReqEventFd,
        &handles->respEventFd,
        &handles->reqCreditEventFd,
        &handles->respCreditEventFd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
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

class CloseSessionStageGate final {
public:
    explicit CloseSessionStageGate(CloseSessionStage targetStage)
        : targetStage_(targetStage)
    {
    }

    std::function<void(CloseSessionStage)> Hook()
    {
        return [this](CloseSessionStage stage) { OnStage(stage); };
    }

    bool WaitUntilEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    void OnStage(CloseSessionStage stage)
    {
        if (stage != targetStage_) {
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    const CloseSessionStage targetStage_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

class ScopedEnvVar final {
public:
    ScopedEnvVar(const char* name, const char* value)
        : name_(name)
    {
        const char* current = std::getenv(name);
        if (current != nullptr) {
            hadValue_ = true;
            previousValue_ = current;
        }
        setenv(name, value, 1);
    }

    ~ScopedEnvVar()
    {
        if (hadValue_) {
            setenv(name_.c_str(), previousValue_.c_str(), 1);
            return;
        }
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::string previousValue_;
    bool hadValue_ = false;
};

}  // namespace

TEST(VesShutdownLifecycleTest, CloseSessionRejectsReopenUntilServerStops)
{
    BlockingRegistrar registrar;
    auto sessionService = std::make_shared<EngineSessionService>(std::vector<RpcHandlerRegistrar*>{&registrar});
    auto bootstrap = std::make_shared<SessionServiceBootstrapChannel>(sessionService);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kBlockingOpcode;
    call.execTimeoutMs = 200;
    auto future = client.InvokeAsync(call);

    ASSERT_TRUE(registrar.WaitUntilRunning(std::chrono::milliseconds(200)));

    std::atomic<bool> closeDone{false};
    std::thread closeThread([&]() {
        EXPECT_EQ(sessionService->CloseSession(), MemRpc::StatusCode::Ok);
        closeDone.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_FALSE(closeDone.load(std::memory_order_acquire));

    MemRpc::BootstrapHandles reopened = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService->OpenSession(reopened), MemRpc::StatusCode::PeerDisconnected);
    CloseHandles(&reopened);

    registrar.Release();

    MemRpc::RpcReply reply;
    const MemRpc::StatusCode waitStatus = std::move(future).Wait(&reply);
    EXPECT_TRUE(waitStatus == MemRpc::StatusCode::Ok || waitStatus == MemRpc::StatusCode::ExecTimeout ||
                waitStatus == MemRpc::StatusCode::PeerDisconnected);

    closeThread.join();
    EXPECT_TRUE(closeDone.load(std::memory_order_acquire));

    MemRpc::BootstrapHandles fresh = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService->OpenSession(fresh), MemRpc::StatusCode::Ok);
    CloseHandles(&fresh);

    client.Shutdown();
}

TEST(VesShutdownLifecycleTest, OnStopDefersEngineResetUntilSessionDrainCompletes)
{
    auto service = std::make_shared<VirusExecutorService>();
    service->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(service->AsObject());
    ASSERT_NE(control, nullptr);

    VesOpenSessionRequest request = DefaultVesOpenSessionRequest();
    request.engineKinds = {static_cast<uint32_t>(VesEngineKind::Scan), 99u};
    auto bootstrap = std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control), request);
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep300_shutdown.bin");
    ASSERT_TRUE(WaitFor(
        [&]() {
            VesHeartbeatReply reply{};
            return service->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
        },
        std::chrono::milliseconds(200)));

    std::atomic<bool> stopDone{false};
    std::thread stopThread([&]() {
        service->OnStop();
        stopDone.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_FALSE(stopDone.load(std::memory_order_acquire));
    EXPECT_EQ(service->service().configuredEngineKinds(),
              (std::vector<uint32_t>{static_cast<uint32_t>(VesEngineKind::Scan), 99u}));

    MemRpc::RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), MemRpc::StatusCode::PeerDisconnected);

    stopThread.join();
    EXPECT_TRUE(stopDone.load(std::memory_order_acquire));
    EXPECT_TRUE(service->service().configuredEngineKinds().empty());

    client.Shutdown();
}

TEST(VesShutdownLifecycleTest, CloseSessionChaosRejectsReopenStormUntilRpcServerStopCompletes)
{
    BlockingRegistrar registrar;
    CloseSessionStageGate gate(CloseSessionStage::BeforeRpcServerStop);
    auto sessionService = std::make_shared<EngineSessionService>(
        std::vector<RpcHandlerRegistrar*>{&registrar}, nullptr, EngineSessionServiceOptions{gate.Hook()});
    auto bootstrap = std::make_shared<SessionServiceBootstrapChannel>(sessionService);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kBlockingOpcode;
    call.execTimeoutMs = 200;
    auto future = client.InvokeAsync(call);

    ASSERT_TRUE(registrar.WaitUntilRunning(std::chrono::milliseconds(200)));

    std::atomic<bool> closeDone{false};
    std::thread closeThread([&]() {
        EXPECT_EQ(sessionService->CloseSession(), MemRpc::StatusCode::Ok);
        closeDone.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(gate.WaitUntilEntered(std::chrono::milliseconds(200)));
    ASSERT_FALSE(closeDone.load(std::memory_order_acquire));

    registrar.Release();

    constexpr int kStormThreads = 8;
    constexpr int kStormAttempts = 20;
    std::atomic<int> peerDisconnectedCount{0};
    std::atomic<int> unexpectedCount{0};
    std::vector<std::thread> stormThreads;
    stormThreads.reserve(kStormThreads);
    for (int threadIndex = 0; threadIndex < kStormThreads; ++threadIndex) {
        stormThreads.emplace_back([&]() {
            for (int attempt = 0; attempt < kStormAttempts; ++attempt) {
                MemRpc::BootstrapHandles reopened = MemRpc::MakeDefaultBootstrapHandles();
                const MemRpc::StatusCode status = sessionService->OpenSession(reopened);
                if (status == MemRpc::StatusCode::PeerDisconnected) {
                    ++peerDisconnectedCount;
                } else {
                    ++unexpectedCount;
                }
                CloseHandles(&reopened);
                std::this_thread::yield();
            }
        });
    }
    for (auto& stormThread : stormThreads) {
        stormThread.join();
    }
    EXPECT_EQ(peerDisconnectedCount.load(), kStormThreads * kStormAttempts);
    EXPECT_EQ(unexpectedCount.load(), 0);

    gate.Release();
    closeThread.join();
    EXPECT_TRUE(closeDone.load(std::memory_order_acquire));

    MemRpc::RpcReply reply;
    const MemRpc::StatusCode waitStatus = std::move(future).Wait(&reply);
    EXPECT_TRUE(waitStatus == MemRpc::StatusCode::Ok || waitStatus == MemRpc::StatusCode::ExecTimeout ||
                waitStatus == MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles fresh = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService->OpenSession(fresh), MemRpc::StatusCode::Ok);
    CloseHandles(&fresh);

    client.Shutdown();
}

TEST(VesShutdownLifecycleTest, ConcurrentCloseSessionCallsRemainIdempotentWhenFirstCloseBlocks)
{
    BlockingRegistrar registrar;
    CloseSessionStageGate gate(CloseSessionStage::BeforeRpcServerStop);
    auto sessionService = std::make_shared<EngineSessionService>(
        std::vector<RpcHandlerRegistrar*>{&registrar}, nullptr, EngineSessionServiceOptions{gate.Hook()});
    auto bootstrap = std::make_shared<SessionServiceBootstrapChannel>(sessionService);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    MemRpc::RpcCall call;
    call.opcode = kBlockingOpcode;
    call.execTimeoutMs = 200;
    auto future = client.InvokeAsync(call);

    ASSERT_TRUE(registrar.WaitUntilRunning(std::chrono::milliseconds(200)));

    std::atomic<bool> firstCloseDone{false};
    std::thread firstClose([&]() {
        EXPECT_EQ(sessionService->CloseSession(), MemRpc::StatusCode::Ok);
        firstCloseDone.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(gate.WaitUntilEntered(std::chrono::milliseconds(200)));
    ASSERT_FALSE(firstCloseDone.load(std::memory_order_acquire));

    std::atomic<bool> secondCloseDone{false};
    std::thread secondClose([&]() {
        EXPECT_EQ(sessionService->CloseSession(), MemRpc::StatusCode::Ok);
        secondCloseDone.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(WaitFor([&]() { return secondCloseDone.load(std::memory_order_acquire); }, std::chrono::milliseconds(200)));
    ASSERT_FALSE(firstCloseDone.load(std::memory_order_acquire));

    registrar.Release();
    gate.Release();

    firstClose.join();
    secondClose.join();
    EXPECT_TRUE(firstCloseDone.load(std::memory_order_acquire));
    EXPECT_TRUE(secondCloseDone.load(std::memory_order_acquire));

    MemRpc::RpcReply reply;
    const MemRpc::StatusCode waitStatus = std::move(future).Wait(&reply);
    EXPECT_TRUE(waitStatus == MemRpc::StatusCode::ExecTimeout || waitStatus == MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles fresh = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService->OpenSession(fresh), MemRpc::StatusCode::Ok);
    CloseHandles(&fresh);

    client.Shutdown();
}

TEST(VesShutdownLifecycleTest, CloseSessionChaosEnvironmentExtendsIdleCloseWindow)
{
    ScopedEnvVar enableChaos("VES_ENABLE_CLOSE_SESSION_CHAOS", "1");
    ScopedEnvVar sleepMs("VES_CLOSE_SESSION_CHAOS_SLEEP_MS", "25");
    ScopedEnvVar yieldCount("VES_CLOSE_SESSION_CHAOS_YIELDS", "2");

    VesEngineService service;
    EngineSessionService sessionService({&service});
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(&handles);

    std::atomic<bool> closeDone{false};
    std::thread closeThread([&]() {
        EXPECT_EQ(sessionService.CloseSession(), MemRpc::StatusCode::Ok);
        closeDone.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(closeDone.load(std::memory_order_acquire));

    MemRpc::BootstrapHandles reopened = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService.OpenSession(reopened), MemRpc::StatusCode::PeerDisconnected);
    CloseHandles(&reopened);

    closeThread.join();
    EXPECT_TRUE(closeDone.load(std::memory_order_acquire));

    MemRpc::BootstrapHandles fresh = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService.OpenSession(fresh), MemRpc::StatusCode::Ok);
    CloseHandles(&fresh);
    EXPECT_EQ(sessionService.CloseSession(), MemRpc::StatusCode::Ok);
}

}  // namespace VirusExecutorService
