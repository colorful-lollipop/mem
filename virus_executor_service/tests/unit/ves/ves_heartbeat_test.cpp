#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"
#include "transport/ves_control_proxy.h"
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

}  // namespace

TEST(VesHeartbeatTest, UnhealthyBeforeOpenSession) {
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

TEST(VesHeartbeatTest, OkAfterOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles{};
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
    EXPECT_EQ(reply.flags,
              VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED);

    service.CloseSession();
    service.OnStop();
}

TEST(VesHeartbeatTest, HeartbeatOverSaSocket) {
    const std::string socketPath = "/tmp/virus_executor_service_hb_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VesControlProxy proxy(stub->AsObject(), socketPath);
    VesOpenSessionRequest request;
    request.engineKinds = {99u, static_cast<uint32_t>(VesEngineKind::Scan), 99u};
    MemRpc::BootstrapHandles handles{};
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

TEST(VesHeartbeatTest, OpenSessionRejectsDifferentEngineListAfterConfiguration) {
    VirusExecutorService service;
    service.OnStart();

    VesOpenSessionRequest firstRequest;
    firstRequest.engineKinds = {99u};

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(firstRequest, handles), MemRpc::StatusCode::Ok);
    EXPECT_EQ(service.service().configuredEngineKinds(), (std::vector<uint32_t>{99u}));

    VesOpenSessionRequest secondRequest;
    secondRequest.engineKinds = {static_cast<uint32_t>(VesEngineKind::Scan)};

    MemRpc::BootstrapHandles rejected{};
    EXPECT_EQ(service.OpenSession(secondRequest, rejected), MemRpc::StatusCode::InvalidArgument);

    ScanTask scanTask{"/data/eicar.bin"};
    const ScanFileReply reply = service.service().ScanFile(scanTask);
    EXPECT_EQ(reply.code, -1);

    service.CloseSession();
    CloseHandles(&handles);
    service.OnStop();
}

TEST(VesHeartbeatTest, BootstrapChannelWorksWithInterfaceOnlyControl) {
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVesControl>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(control, DefaultVesOpenSessionRequest());
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto result = bootstrap.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Healthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);

    EXPECT_EQ(bootstrap.CloseSession(), MemRpc::StatusCode::Ok);
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesHealthyReply) {
    const std::string socketPath = "/tmp/virus_executor_service_health_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    auto control = OHOS::iface_cast<IVesControl>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(control, DefaultVesOpenSessionRequest());
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto result = bootstrap.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Healthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);

    bootstrap.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesUnhealthyAndSessionMismatchReplies) {
    const std::string socketPath =
        "/tmp/virus_executor_service_health_mismatch_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    auto control = OHOS::iface_cast<IVesControl>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    VesBootstrapChannel bootstrap(control, DefaultVesOpenSessionRequest());

    const auto unhealthy = bootstrap.CheckHealth(42);
    EXPECT_EQ(unhealthy.status, MemRpc::ChannelHealthStatus::Unhealthy);
    EXPECT_EQ(unhealthy.sessionId, 0u);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto mismatch = bootstrap.CheckHealth(handles.sessionId + 1);
    EXPECT_EQ(mismatch.status, MemRpc::ChannelHealthStatus::SessionMismatch);
    EXPECT_EQ(mismatch.sessionId, handles.sessionId);

    bootstrap.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, HeartbeatShowsInFlight) {
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVesControl>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap = std::make_shared<VesBootstrapChannel>(control, DefaultVesOpenSessionRequest());
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep50.bin");
    ASSERT_TRUE(WaitFor([&]() {
        VesHeartbeatReply reply{};
        return stub->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
    }, std::chrono::milliseconds(200)));

    VesHeartbeatReply reply{};
    EXPECT_EQ(stub->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_GE(reply.inFlight, 1u);
    EXPECT_STREQ(reply.currentTask, "active");
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_BUSY, 0u);

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(future.Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    stub->OnStop();
}

TEST(VesHeartbeatTest, LongRunningHeartbeatIsDegraded) {
    auto stub = std::make_shared<VirusExecutorService>();
    stub->OnStart();

    auto control = OHOS::iface_cast<IVesControl>(stub->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap = std::make_shared<VesBootstrapChannel>(control, DefaultVesOpenSessionRequest());
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep200_long.bin");
    ASSERT_TRUE(WaitFor([&]() {
        VesHeartbeatReply reply{};
        return stub->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
    }, std::chrono::milliseconds(200)));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    VesHeartbeatReply reply{};
    EXPECT_EQ(stub->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_LONG_RUNNING, 0u);
    EXPECT_GE(reply.lastTaskAgeMs, 100u);

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(future.Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    stub->OnStop();
}

}  // namespace VirusExecutorService
