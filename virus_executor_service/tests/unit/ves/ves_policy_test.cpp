#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_map>

#define private public
#include "client/ves_client.h"
#undef private
#include "client/internal/ves_client_recovery_access.h"
#include "isam_backend.h"
#include "iservice_registry.h"
#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/core/codec.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "service/virus_executor_service.h"
#include "system_ability.h"
#include "transport/ves_control_stub.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

namespace {

class InMemorySamBackend final : public OHOS::ISamBackend {
public:
    std::string GetServicePath(int32_t saId) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = servicePaths_.find(saId);
        return it == servicePaths_.end() ? std::string{} : it->second;
    }

    std::string LoadService(int32_t saId) override
    {
        return GetServicePath(saId);
    }

    bool AddService(int32_t saId, const std::string& servicePath) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        servicePaths_[saId] = servicePath;
        return true;
    }

    bool UnloadService(int32_t saId) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return servicePaths_.erase(saId) > 0;
    }

private:
    std::mutex mutex_;
    std::unordered_map<int32_t, std::string> servicePaths_;
};

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

void UnloadControlService()
{
    auto& samClient = OHOS::SystemAbilityManagerClient::GetInstance();
    auto sam = samClient.GetSystemAbilityManager();
    if (sam != nullptr) {
        (void)sam->UnloadSystemAbility(VIRUS_PROTECTION_EXECUTOR_SA_ID);
    }
    samClient.SetBackend(nullptr);
}

void UseInMemorySamBackend()
{
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(std::make_shared<InMemorySamBackend>());
}

void RequestRecoveryForTest(VesClient& client, uint32_t delayMs)
{
    client.client_.RequestExternalRecovery({
        0,
        delayMs,
    });
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
};

class FakeHealthControlService final : public OHOS::SystemAbility, public VesControlStub {
public:
    FakeHealthControlService()
        : OHOS::SystemAbility(VIRUS_PROTECTION_EXECUTOR_SA_ID, true),
          bootstrap_(std::make_shared<MemRpc::DevBootstrapChannel>())
    {
    }

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles) override
    {
        EXPECT_TRUE(request.engineKinds.empty());
        openCount_.fetch_add(1);
        sessionOpen_.store(true);
        return bootstrap_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        closeCount_.fetch_add(1);
        sessionOpen_.store(false);
        return bootstrap_->CloseSession();
    }

    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override
    {
        reply = {};
        reply.version = 2;
        reply.sessionId = sessionOpen_.load() ? bootstrap_->serverHandles().sessionId : 0;
        std::snprintf(reply.currentTask, sizeof(reply.currentTask), "%s", "idle");
        if (sessionOpen_.load() && healthy_.load()) {
            reply.status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
            reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::None);
            reply.flags = VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED;
        } else {
            reply.status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession);
            reply.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession);
        }
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode AnyCall(const VesAnyCallRequest& request, VesAnyCallReply& reply) override
    {
        if (!sessionOpen_.load()) {
            return MemRpc::StatusCode::PeerDisconnected;
        }

        reply = {};
        if (static_cast<VesOpcode>(request.opcode) != VesOpcode::ScanFile) {
            reply.status = MemRpc::StatusCode::InvalidArgument;
            return MemRpc::StatusCode::Ok;
        }

        ScanTask scanTask;
        if (!MemRpc::DecodeMessage<ScanTask>(request.payload, &scanTask)) {
            reply.status = MemRpc::StatusCode::ProtocolMismatch;
            return MemRpc::StatusCode::Ok;
        }

        ScanFileReply scanReply;
        scanReply.code = 0;
        scanReply.threatLevel = scanTask.path.find("recovered") != std::string::npos ? 1 : 0;
        reply.status = MemRpc::StatusCode::Ok;
        if (!MemRpc::EncodeMessage<ScanFileReply>(scanReply, &reply.payload)) {
            reply.status = MemRpc::StatusCode::EngineInternalError;
        }
        return MemRpc::StatusCode::Ok;
    }

    void OnStart() override
    {
    }

    void OnStop() override
    {
        sessionOpen_.store(false);
        (void)bootstrap_->CloseSession();
        OHOS::SystemAbility::OnStop();
    }

    void PrepareServerHandlesForTest()
    {
        MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(bootstrap_->OpenSession(handles), MemRpc::StatusCode::Ok);
        CloseHandles(handles);
    }

    void SetServicePathForTest(const std::string& socketPath)
    {
        AsObject()->SetServicePath(socketPath);
    }

    [[nodiscard]] MemRpc::BootstrapHandles serverHandles() const
    {
        return bootstrap_->serverHandles();
    }

    void SetHealthy(bool healthy)
    {
        healthy_.store(healthy);
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
    std::shared_ptr<MemRpc::DevBootstrapChannel> bootstrap_;
    std::atomic<bool> healthy_{true};
    std::atomic<bool> sessionOpen_{false};
    std::atomic<int> openCount_{0};
    std::atomic<int> closeCount_{0};
};

}  // namespace

TEST(VesPolicyTest, ExecTimeoutTriggersOnFailure)
{
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unused = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused), MemRpc::StatusCode::Ok);

    MemRpc::RpcServer server(bootstrap->serverHandles());
    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
        &server,
        static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanTask& req) {
            // Force exec timeout by sleeping longer than client timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = 0;
            (void)req;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    std::atomic<bool> failureCalled{false};
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [&](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            failureCalled.store(true);
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanTask req;
    req.path = "/data/sleep50.bin";

    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(VesOpcode::ScanFile);
    call.execTimeoutMs = 5;  // short timeout
    MemRpc::CodecTraits<ScanTask>::Encode(req, &call.payload);

    auto future = client.InvokeAsync(call);
    MemRpc::RpcReply reply;
    auto status = std::move(future).Wait(&reply);

    EXPECT_EQ(status, MemRpc::StatusCode::ExecTimeout);
    EXPECT_TRUE(failureCalled.load());

    client.Shutdown();
    server.Stop();
}

TEST(VesPolicyTest, CurrentControlReturnsNullWhenBootstrapChannelIsGone)
{
    auto stale = std::make_shared<FakeReloadControl>(100);
    std::atomic<int> loadCount{0};
    VesClient client([&]() -> OHOS::sptr<IVirusProtectionExecutor> {
        loadCount.fetch_add(1);
        return nullptr;
    });

    client.bootstrapChannel_ =
        std::make_shared<VesBootstrapChannel>([stale]() -> OHOS::sptr<IVirusProtectionExecutor> { return stale; },
                                              DefaultVesOpenSessionRequest());

    EXPECT_EQ(client.CurrentControl(), stale);

    client.bootstrapChannel_.reset();

    EXPECT_EQ(client.CurrentControl(), nullptr);
    EXPECT_EQ(loadCount.load(), 0);
}

TEST(VesPolicyTest, DefaultOptionsEnableOneMinuteIdleShutdown)
{
    const VesClientOptions options;
    EXPECT_EQ(options.idleShutdownTimeoutMs, 60000u);
    EXPECT_FALSE(options.recoveryPolicy.onFailure);
    EXPECT_FALSE(options.recoveryPolicy.onIdle);
    EXPECT_FALSE(options.recoveryPolicy.onEngineDeath);
}

TEST(VesPolicyTest, IdleShutdownClosesSessionAndReopensOnDemand)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_idle_policy_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    VesClientOptions options;
    options.idleShutdownTimeoutMs = 80;
    auto client = VesClient::Connect(options);
    ASSERT_NE(client, nullptr);

    ScanTask cleanTask{"/data/clean.apk"};
    ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(cleanTask, &reply), MemRpc::StatusCode::Ok);

    VesHeartbeatReply heartbeat{};
    const auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < idleDeadline) {
        ASSERT_EQ(service->Heartbeat(heartbeat), MemRpc::StatusCode::Ok);
        if (heartbeat.sessionId == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(heartbeat.sessionId, 0u);
    const auto idleSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(idleSnapshot.lifecycleState, MemRpc::ClientLifecycleState::IdleClosed);

    ScanTask reopenTask{"/data/reopen.apk"};
    ASSERT_EQ(client->ScanFile(reopenTask, &reply), MemRpc::StatusCode::Ok);

    ASSERT_EQ(service->Heartbeat(heartbeat), MemRpc::StatusCode::Ok);
    EXPECT_NE(heartbeat.sessionId, 0u);
    const auto reopenedSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(reopenedSnapshot.lifecycleState, MemRpc::ClientLifecycleState::Active);

    client->Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, VesClientRecoversFromHeartbeatFailureWithoutClientLoop)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_watchdog_policy_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<FakeHealthControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(&server,
                                                          static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
                                                          [](const ScanTask&) {
                                                              ScanFileReply reply;
                                                              reply.code = 0;
                                                              reply.threatLevel = 0;
                                                              return reply;
                                                          });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    auto client = VesClient::Connect();
    ASSERT_NE(client, nullptr);

    ScanTask healthyTask{"/data/healthy.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(healthyTask, &reply), MemRpc::StatusCode::Ok);

    service->SetHealthy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->closeCount() >= 1; }, std::chrono::milliseconds(500)));
    service->SetHealthy(true);

    ScanTask recoveredTask{"/data/recovered.bin"};
    ASSERT_EQ(client->ScanFile(recoveredTask, &reply), MemRpc::StatusCode::Ok);
    const auto postRecoverySnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(postRecoverySnapshot.lifecycleState, MemRpc::ClientLifecycleState::Active);

    client->Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, VesClientScanFileRetriesAcrossRestartCooldown)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_restart_cooldown_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<FakeHealthControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    auto registerScanHandler = [](MemRpc::RpcServer* server) {
        MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
            server,
            static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
            [](const ScanTask& request) {
                ScanFileReply reply;
                reply.code = 0;
                reply.threatLevel = request.path.find("recovered") != std::string::npos ? 1 : 0;
                return reply;
            });
    };

    MemRpc::RpcServer server(service->serverHandles());
    registerScanHandler(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    VesClientOptions options;
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 120};
    };
    auto client = VesClient::Connect(options);
    ASSERT_NE(client, nullptr);

    ScanTask initialTask{"/data/healthy.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(initialTask, &reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 0);

    RequestRecoveryForTest(*client, 120);
    const auto cooldownSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(cooldownSnapshot.lifecycleState, MemRpc::ClientLifecycleState::Cooldown);
    EXPECT_TRUE(cooldownSnapshot.recoveryPending);

    ScanTask recoveredTask{"/data/recovered.bin"};
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(client->ScanFile(recoveredTask, &reply), MemRpc::StatusCode::Ok);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    EXPECT_EQ(reply.threatLevel, 1);
    EXPECT_GE(elapsed.count(), 80);
    EXPECT_LT(elapsed.count(), 1000);
    const auto activeSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_TRUE(activeSnapshot.lifecycleState == MemRpc::ClientLifecycleState::Active ||
                activeSnapshot.lifecycleState == MemRpc::ClientLifecycleState::Cooldown);

    client->Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, VesClientScanFileHonorsRequestedRecoveryDelayBeyondConfiguredBudget)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_restart_long_cooldown_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<FakeHealthControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    MemRpc::RpcServer server(service->serverHandles());
    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
        &server,
        static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanTask& request) {
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = request.path.find("recovered") != std::string::npos ? 1 : 0;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    VesClientOptions options;
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 120};
    };
    auto client = VesClient::Connect(options);
    ASSERT_NE(client, nullptr);

    RequestRecoveryForTest(*client, 400);
    const auto cooldownSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(cooldownSnapshot.lifecycleState, MemRpc::ClientLifecycleState::Cooldown);
    EXPECT_TRUE(cooldownSnapshot.recoveryPending);
    EXPECT_GE(cooldownSnapshot.cooldownRemainingMs, 250u);

    ScanTask recoveredTask{"/data/recovered.bin"};
    ScanFileReply reply;
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(client->ScanFile(recoveredTask, &reply), MemRpc::StatusCode::Ok);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    EXPECT_EQ(reply.threatLevel, 1);
    EXPECT_GE(elapsed.count(), 300);
    EXPECT_LT(elapsed.count(), 1500);

    client->Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, VesClientUsesRpcClientRecoveryInvokeForAnyCallFallback)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_restart_anycall_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<FakeHealthControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    VesClientOptions options;
    options.recoveryPolicy.onEngineDeath = [](const MemRpc::EngineDeathReport&) {
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, 120};
    };
    auto client = VesClient::Connect(options);
    ASSERT_NE(client, nullptr);

    RequestRecoveryForTest(*client, 120);

    std::thread reopenThread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MemRpc::BootstrapHandles reopened = MemRpc::MakeDefaultBootstrapHandles();
        if (service->OpenSession(DefaultVesOpenSessionRequest(), reopened) == MemRpc::StatusCode::Ok) {
            CloseHandles(reopened);
        }
    });

    const std::string longPath = "/data/recovered_" + std::string(MemRpc::DEFAULT_MAX_REQUEST_BYTES + 64, 'b');
    ScanTask recoveredTask{longPath};
    ScanFileReply reply;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client->ScanFile(recoveredTask, &reply), MemRpc::StatusCode::Ok);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    EXPECT_EQ(reply.threatLevel, 1);
    EXPECT_GE(elapsed.count(), 80);
    EXPECT_LT(elapsed.count(), 1000);
    const auto activeSnapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_TRUE(activeSnapshot.lifecycleState == MemRpc::ClientLifecycleState::Active ||
                activeSnapshot.lifecycleState == MemRpc::ClientLifecycleState::Cooldown);

    reopenThread.join();
    client->Shutdown();
    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, ShutdownKeepsVesClientTerminal)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_shutdown_terminal_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<VirusExecutorService>();
    service->AsObject()->SetServicePath(socketPath);
    service->OnStart();
    ASSERT_TRUE(service->Publish(service.get()));

    auto client = VesClient::Connect();
    ASSERT_NE(client, nullptr);
    client->Shutdown();
    EXPECT_EQ(client->Init(), MemRpc::StatusCode::ClientClosed);

    const auto snapshot = internal::VesClientRecoveryAccess::GetRecoveryRuntimeSnapshot(*client);
    EXPECT_EQ(snapshot.lifecycleState, MemRpc::ClientLifecycleState::Closed);

    ScanTask task{"/data/after_shutdown.apk"};
    ScanFileReply reply;
    EXPECT_EQ(client->ScanFile(task, &reply), MemRpc::StatusCode::ClientClosed);

    service->OnStop();
    UnloadControlService();
}

TEST(VesPolicyTest, VesClientScanFileForwardsPriority)
{
    UnloadControlService();
    const std::string socketPath = "/tmp/ves_priority_policy_" + std::to_string(getpid()) + ".sock";
    UseInMemorySamBackend();

    auto service = std::make_shared<FakeHealthControlService>();
    service->SetServicePathForTest(socketPath);
    service->PrepareServerHandlesForTest();
    ASSERT_TRUE(service->Publish(service.get()));

    std::atomic<int> invocationCount{0};
    std::atomic<int> lastPriority{static_cast<int>(MemRpc::Priority::Normal)};
    MemRpc::RpcServer server(service->serverHandles());
    server.RegisterHandler(static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
                           [&](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                               if (reply == nullptr) {
                                   return;
                               }
                               ScanTask request;
                               if (!MemRpc::DecodeMessage<ScanTask>(call.payload, &request)) {
                                   reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                   return;
                               }
                               ScanFileReply scanReply;
                               scanReply.code = 0;
                               scanReply.threatLevel = request.path.find("high") != std::string::npos ? 1 : 0;
                               if (!MemRpc::EncodeMessage<ScanFileReply>(scanReply, &reply->payload)) {
                                   reply->status = MemRpc::StatusCode::EngineInternalError;
                                   return;
                               }
                               invocationCount.fetch_add(1);
                               lastPriority.store(static_cast<int>(call.priority));
                           });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    auto client = VesClient::Connect();
    ASSERT_NE(client, nullptr);

    ScanTask normalTask{"/data/normal.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client->ScanFile(normalTask, &reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 0);
    EXPECT_EQ(lastPriority.load(), static_cast<int>(MemRpc::Priority::Normal));

    ScanTask highTask{"/data/high.bin"};
    ASSERT_EQ(client->ScanFile(highTask, &reply, MemRpc::Priority::High), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 1);
    EXPECT_EQ(lastPriority.load(), static_cast<int>(MemRpc::Priority::High));
    EXPECT_EQ(invocationCount.load(), 2);

    client->Shutdown();
    server.Stop();
    service->OnStop();
    UnloadControlService();
}

}  // namespace VirusExecutorService
