#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"
#include "transport/ves_control_proxy.h"
#include "transport/ves_control_stub.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

namespace {

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
    EXPECT_TRUE(MemRpc::EncodeMessage<ScanTask>(task, &call.payload));
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

class FakeReloadControl final : public VesControlStub {
public:
    explicit FakeReloadControl(uint64_t sessionBase)
        : sessionBase_(sessionBase)
    {
    }

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles) override
    {
        EXPECT_TRUE(request.engineKinds.empty());
        handles = MemRpc::MakeDefaultBootstrapHandles();
        handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
        handles.sessionId = ++sessionBase_;
        openCount_.fetch_add(1);
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode CloseSession() override
    {
        closeCount_.fetch_add(1);
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override
    {
        reply = {};
        reply.version = 2;
        reply.status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
        reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::None);
        reply.flags = VES_HEARTBEAT_FLAG_INITIALIZED;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode AnyCall(const VesAnyCallRequest&, VesAnyCallReply&) override
    {
        return MemRpc::StatusCode::InvalidArgument;
    }

    [[nodiscard]] int openCount() const
    {
        return openCount_.load();
    }

    [[nodiscard]] int closeCount() const
    {
        return closeCount_.load();
    }

private:
    std::atomic<uint64_t> sessionBase_;
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
};

class TrackingRemoteObject final : public OHOS::IRemoteObject {
public:
    bool AddDeathRecipient(const OHOS::sptr<DeathRecipient>& recipient) override
    {
        const int active = activeAdds_.fetch_add(1) + 1;
        int observed = maxConcurrentAdds_.load();
        while (active > observed && !maxConcurrentAdds_.compare_exchange_weak(observed, active)) {
        }
        addCount_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const bool added = OHOS::IRemoteObject::AddDeathRecipient(recipient);
        activeAdds_.fetch_sub(1);
        return added;
    }

    [[nodiscard]] int addCount() const
    {
        return addCount_.load();
    }

    [[nodiscard]] int maxConcurrentAdds() const
    {
        return maxConcurrentAdds_.load();
    }

private:
    std::atomic<int> activeAdds_{0};
    std::atomic<int> addCount_{0};
    std::atomic<int> maxConcurrentAdds_{0};
};

class TrackingControl final : public IVirusProtectionExecutor {
public:
    TrackingControl(uint64_t sessionBase, OHOS::sptr<TrackingRemoteObject> remote)
        : sessionBase_(sessionBase),
          remote_(std::move(remote))
    {
    }

    OHOS::sptr<OHOS::IRemoteObject> AsObject() override
    {
        return remote_;
    }

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles) override
    {
        EXPECT_TRUE(request.engineKinds.empty());
        handles = MemRpc::MakeDefaultBootstrapHandles();
        handles.protocolVersion = MemRpc::PROTOCOL_VERSION;
        handles.sessionId = ++sessionBase_;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode CloseSession() override
    {
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override
    {
        reply = {};
        reply.version = 2;
        reply.status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
        reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::None);
        reply.flags = VES_HEARTBEAT_FLAG_INITIALIZED;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode AnyCall(const VesAnyCallRequest&, VesAnyCallReply&) override
    {
        return MemRpc::StatusCode::InvalidArgument;
    }

private:
    std::atomic<uint64_t> sessionBase_;
    OHOS::sptr<TrackingRemoteObject> remote_;
};

}  // namespace

TEST(VesHeartbeatTest, UnhealthyBeforeOpenSession)
{
    VirusExecutorService service;
    service.OnStart();

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.version, 2u);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession));
    EXPECT_EQ(reply.flags, VES_HEARTBEAT_FLAG_INITIALIZED);

    service.OnStop();
}

TEST(VesHeartbeatTest, OkAfterOpenSession)
{
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(service.OpenSession(DefaultVesOpenSessionRequest(), handles), MemRpc::StatusCode::Ok);
    const uint64_t session_id = handles.sessionId;

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkIdle));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::None));
    EXPECT_EQ(reply.sessionId, session_id);
    EXPECT_STREQ(reply.currentTask, "idle");
    EXPECT_EQ(reply.version, 2u);
    EXPECT_EQ(reply.inFlight, 0u);
    EXPECT_EQ(reply.lastTaskAgeMs, 0u);
    EXPECT_EQ(reply.flags, VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED);

    service.CloseSession();
    service.OnStop();
}

TEST(VesHeartbeatTest, HeartbeatOverSaSocket)
{
    const std::string socketPath = "/tmp/virus_executor_service_hb_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VesControlProxy proxy(stub->AsObject(), socketPath);
    VesOpenSessionRequest request;
    request.engineKinds = {99u, static_cast<uint32_t>(VesEngineKind::Scan), 99u};
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(proxy.OpenSession(request, handles), MemRpc::StatusCode::Ok);
    const uint64_t session_id = handles.sessionId;
    EXPECT_EQ(stub->service().configuredEngineKinds(),
              (std::vector<uint32_t>{static_cast<uint32_t>(VesEngineKind::Scan), 99u}));

    VesHeartbeatReply reply{};
    EXPECT_EQ(proxy.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkIdle));
    EXPECT_EQ(reply.sessionId, session_id);

    proxy.CloseSession();
    CloseHandles(&handles);
    stub->OnStop();
}

TEST(VesHeartbeatTest, OpenSessionRejectsDifferentEngineListAfterConfiguration)
{
    VirusExecutorService service;
    service.OnStart();

    VesOpenSessionRequest firstRequest;
    firstRequest.engineKinds = {99u};

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(service.OpenSession(firstRequest, handles), MemRpc::StatusCode::Ok);
    EXPECT_EQ(service.service().configuredEngineKinds(), (std::vector<uint32_t>{99u}));

    VesOpenSessionRequest secondRequest;
    secondRequest.engineKinds = {static_cast<uint32_t>(VesEngineKind::Scan)};

    MemRpc::BootstrapHandles rejected = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(service.OpenSession(secondRequest, rejected), MemRpc::StatusCode::InvalidArgument);

    ScanTask scanTask{"/data/eicar.bin"};
    const ScanFileReply reply = service.service().ScanFile(scanTask);
    EXPECT_EQ(reply.code, -1);

    service.CloseSession();
    CloseHandles(&handles);
    service.OnStop();
}

TEST(VesHeartbeatTest, BootstrapChannelWorksWithInterfaceOnlyControl)
{
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto result = bootstrap.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Healthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);

    EXPECT_EQ(bootstrap.CloseSession(), MemRpc::StatusCode::Ok);
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesHealthyReply)
{
    const std::string socketPath = "/tmp/virus_executor_service_health_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto result = bootstrap.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Healthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);

    bootstrap.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesUnhealthyAndSessionMismatchReplies)
{
    const std::string socketPath = "/tmp/virus_executor_service_health_mismatch_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    ASSERT_NE(bootstrap.CurrentControl(), nullptr);

    const auto unhealthy = bootstrap.CheckHealth(42);
    EXPECT_EQ(unhealthy.status, MemRpc::ChannelHealthStatus::Unhealthy);
    EXPECT_EQ(unhealthy.sessionId, 0u);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto mismatch = bootstrap.CheckHealth(handles.sessionId + 1);
    EXPECT_EQ(mismatch.status, MemRpc::ChannelHealthStatus::SessionMismatch);
    EXPECT_EQ(mismatch.sessionId, handles.sessionId);

    bootstrap.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, BootstrapChannelInstallsDeathRecipientAfterInitialBind)
{
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    std::atomic<int> deathCount{0};
    {
        VesBootstrapChannel bootstrap(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
        bootstrap.SetEngineDeathCallback([&](uint64_t) { deathCount.fetch_add(1); });
        ASSERT_NE(bootstrap.CurrentControl(), nullptr);

        stub->AsObject()->NotifyRemoteDiedForTest();
        ASSERT_TRUE(WaitFor([&]() { return deathCount.load() >= 1; }, std::chrono::milliseconds(50)));
        EXPECT_EQ(deathCount.load(), 1);
    }

    stub->OnStop();
}

TEST(VesHeartbeatTest, OpenSessionUsesLoaderSequenceAcrossReloads)
{
    auto seed = std::make_shared<FakeReloadControl>(100);
    auto fresh = std::make_shared<FakeReloadControl>(200);

    std::atomic<int> loadCount{0};
    VesBootstrapChannel bootstrap(
        [&]() -> OHOS::sptr<IVirusProtectionExecutor> {
            const int index = loadCount.fetch_add(1);
            return index == 0 ? OHOS::sptr<IVirusProtectionExecutor>(seed) : fresh;
        },
        DefaultVesOpenSessionRequest());

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_EQ(loadCount.load(), 1);
    EXPECT_EQ(seed->openCount(), 1);
    EXPECT_EQ(fresh->openCount(), 0);
    EXPECT_EQ(bootstrap.CurrentControl(), seed);

    EXPECT_EQ(bootstrap.CloseSession(), MemRpc::StatusCode::Ok);
    EXPECT_EQ(seed->closeCount(), 1);

    MemRpc::BootstrapHandles reopened = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(reopened), MemRpc::StatusCode::Ok);
    EXPECT_EQ(loadCount.load(), 2);
    EXPECT_EQ(fresh->openCount(), 1);
    EXPECT_EQ(bootstrap.CurrentControl(), fresh);
}

TEST(VesHeartbeatTest, OpenSessionRefreshesDeadControlBeforeCallingOpenSession)
{
    auto seed = std::make_shared<FakeReloadControl>(100);
    auto fresh = std::make_shared<FakeReloadControl>(200);
    seed->AsObject()->NotifyRemoteDiedForTest();

    std::atomic<int> loadCount{0};
    VesBootstrapChannel bootstrap(
        [&]() -> OHOS::sptr<IVirusProtectionExecutor> {
            const int index = loadCount.fetch_add(1);
            return index == 0 ? OHOS::sptr<IVirusProtectionExecutor>(seed) : fresh;
        },
        DefaultVesOpenSessionRequest());

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_EQ(loadCount.load(), 2);
    EXPECT_EQ(seed->openCount(), 0);
    EXPECT_EQ(fresh->openCount(), 1);
    EXPECT_EQ(bootstrap.CurrentControl(), fresh);
}

TEST(VesHeartbeatTest, HeartbeatShowsInFlight)
{
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap =
        std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep50.bin");
    ASSERT_TRUE(WaitFor(
        [&]() {
            VesHeartbeatReply reply{};
            return stub->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
        },
        std::chrono::milliseconds(200)));

    VesHeartbeatReply reply{};
    EXPECT_EQ(stub->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_GE(reply.inFlight, 1u);
    EXPECT_STREQ(reply.currentTask, "active");
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_BUSY, 0u);

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(std::move(future).Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    stub->OnStop();
}

TEST(VesHeartbeatTest, ProxyControlUsesChannelDeathRecipient)
{
    const std::string socketPath = "/tmp/virus_executor_service_death_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    auto control =
        OHOS::sptr<IVirusProtectionExecutor>(std::make_shared<VesControlProxy>(stub->AsObject(), socketPath));
    ASSERT_NE(control, nullptr);

    auto bootstrap =
        std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    std::atomic<int> deathCount{0};
    bootstrap->SetEngineDeathCallback([&](uint64_t) { deathCount.fetch_add(1); });

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);

    stub->AsObject()->NotifyRemoteDiedForTest();
    ASSERT_TRUE(WaitFor([&]() { return deathCount.load() >= 1; }, std::chrono::milliseconds(50)));
    EXPECT_EQ(deathCount.load(), 1);

    CloseHandles(&handles);
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthReturnsUnhealthyAfterControlDeathWithoutHeartbeat)
{
    auto seed = std::make_shared<FakeReloadControl>(300);
    VesBootstrapChannel bootstrap(MakeStaticControlLoader(seed), DefaultVesOpenSessionRequest());

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    seed->AsObject()->NotifyRemoteDiedForTest();

    const auto result = bootstrap.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Unhealthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);
}

TEST(VesHeartbeatTest, EngineDeathInvalidatesControlUntilNextExplicitRefresh)
{
    auto seed = std::make_shared<FakeReloadControl>(300);
    auto freshA = std::make_shared<FakeReloadControl>(400);
    auto freshB = std::make_shared<FakeReloadControl>(500);

    std::vector<OHOS::sptr<IVirusProtectionExecutor>> controls{seed, freshA, freshB};
    std::atomic<int> loadCount{0};
    VesBootstrapChannel bootstrap(
        [&]() -> OHOS::sptr<IVirusProtectionExecutor> {
            const int index = loadCount.fetch_add(1);
            return controls[static_cast<size_t>(std::min(index, 2))];
        },
        DefaultVesOpenSessionRequest());

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_EQ(loadCount.load(), 1);
    EXPECT_EQ(bootstrap.CurrentControl(), seed);

    std::atomic<int> deathCount{0};
    bootstrap.SetEngineDeathCallback([&](uint64_t) { deathCount.fetch_add(1); });

    seed->AsObject()->NotifyRemoteDiedForTest();
    EXPECT_EQ(deathCount.load(), 1);
    EXPECT_EQ(loadCount.load(), 1);
    EXPECT_EQ(bootstrap.CurrentControl(), freshA);
    EXPECT_EQ(loadCount.load(), 2);

    freshA->AsObject()->NotifyRemoteDiedForTest();
    EXPECT_EQ(deathCount.load(), 2);
    EXPECT_EQ(loadCount.load(), 2);
    EXPECT_EQ(bootstrap.CurrentControl(), freshB);
    EXPECT_EQ(loadCount.load(), 3);
}

TEST(VesHeartbeatTest, ConcurrentRefreshSerializesDeathRecipientRegistration)
{
    constexpr int kRefreshRounds = 4;
    constexpr int kRefreshThreads = 8;

    std::vector<OHOS::sptr<TrackingRemoteObject>> remotes;
    std::vector<OHOS::sptr<IVirusProtectionExecutor>> controls;
    remotes.reserve(kRefreshRounds + 1);
    controls.reserve(kRefreshRounds + 1);
    for (int index = 0; index <= kRefreshRounds; ++index) {
        auto remote = OHOS::sptr<TrackingRemoteObject>(std::make_shared<TrackingRemoteObject>());
        remotes.push_back(remote);
        controls.push_back(OHOS::sptr<IVirusProtectionExecutor>(
            std::make_shared<TrackingControl>(100u + static_cast<uint64_t>(index) * 100u, remote)));
    }

    std::atomic<int> loadCount{0};
    VesBootstrapChannel bootstrap(
        [&]() -> OHOS::sptr<IVirusProtectionExecutor> {
            const int index = loadCount.fetch_add(1);
            return controls[static_cast<size_t>(std::min(index, kRefreshRounds))];
        },
        DefaultVesOpenSessionRequest());

    ASSERT_EQ(bootstrap.CurrentControl(), controls.front());
    EXPECT_EQ(remotes.front()->addCount(), 1);
    EXPECT_EQ(remotes.front()->maxConcurrentAdds(), 1);

    for (int round = 0; round < kRefreshRounds; ++round) {
        remotes[static_cast<size_t>(round)]->NotifyRemoteDiedForTest();

        std::atomic<bool> start{false};
        std::vector<OHOS::sptr<IVirusProtectionExecutor>> seenControls(static_cast<size_t>(kRefreshThreads));
        std::vector<std::thread> threads;
        threads.reserve(kRefreshThreads);
        for (int index = 0; index < kRefreshThreads; ++index) {
            threads.emplace_back([&, index]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                seenControls[static_cast<size_t>(index)] = bootstrap.CurrentControl();
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }

        const auto expectedIndex = static_cast<size_t>(round + 1);
        for (const auto& control : seenControls) {
            EXPECT_EQ(control, controls[expectedIndex]);
        }
        EXPECT_EQ(remotes[expectedIndex]->addCount(), 1);
        EXPECT_EQ(remotes[expectedIndex]->maxConcurrentAdds(), 1);
    }

    EXPECT_EQ(loadCount.load(), kRefreshRounds + 1);
}

TEST(VesHeartbeatTest, LongRunningHeartbeatIsDegraded)
{
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap =
        std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control), DefaultVesOpenSessionRequest());
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep200_long.bin");
    ASSERT_TRUE(WaitFor(
        [&]() {
            VesHeartbeatReply reply{};
            return stub->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
        },
        std::chrono::milliseconds(200)));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    VesHeartbeatReply reply{};
    EXPECT_EQ(stub->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_LONG_RUNNING, 0u);
    EXPECT_GE(reply.lastTaskAgeMs, 100u);

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(std::move(future).Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    stub->OnStop();
}

}  // namespace VirusExecutorService
