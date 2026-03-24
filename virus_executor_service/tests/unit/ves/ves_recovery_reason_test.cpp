#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "memrpc/client/rpc_client.h"
#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"
#include "transport/ves_control_proxy.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"

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
    return [control]() -> OHOS::sptr<IVirusProtectionExecutor> {
        return control;
    };
}

}  // namespace

TEST(VesRecoveryReasonTest, NoSessionMapsToUnhealthyNoSession) {
    VirusExecutorService service;
    service.OnStart();

    VesHeartbeatReply reply{};
    ASSERT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyNoSession));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::NoSession));

    service.OnStop();
}

TEST(VesRecoveryReasonTest, BusyMapsToBusyReason) {
    auto service = std::make_shared<VirusExecutorService>();
    service->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(service->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap = std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control));
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep50_reason.bin");
    ASSERT_TRUE(WaitFor([&]() {
        VesHeartbeatReply reply{};
        return service->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
    }, std::chrono::milliseconds(200)));

    VesHeartbeatReply reply{};
    ASSERT_EQ(service->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(future.Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    service->OnStop();
}

TEST(VesRecoveryReasonTest, LongRunningMapsToLongRunningReason) {
    auto service = std::make_shared<VirusExecutorService>();
    service->OnStart();

    auto control = OHOS::iface_cast<IVirusProtectionExecutor>(service->AsObject());
    ASSERT_NE(control, nullptr);

    auto bootstrap = std::make_shared<VesBootstrapChannel>(MakeStaticControlLoader(control));
    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = StartAsyncScan(&client, "/data/sleep200_reason.bin");
    ASSERT_TRUE(WaitFor([&]() {
        VesHeartbeatReply reply{};
        return service->Heartbeat(reply) == MemRpc::StatusCode::Ok && reply.inFlight >= 1u;
    }, std::chrono::milliseconds(200)));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    VesHeartbeatReply reply{};
    ASSERT_EQ(service->Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));

    MemRpc::RpcReply rpcReply;
    EXPECT_EQ(future.Wait(&rpcReply), MemRpc::StatusCode::Ok);
    client.Shutdown();
    service->OnStop();
}

}  // namespace VirusExecutorService
