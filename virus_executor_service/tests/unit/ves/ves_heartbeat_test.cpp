#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"
#include "transport/ves_control_proxy.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

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
    ASSERT_EQ(service.OpenSession(handles), MemRpc::StatusCode::Ok);
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
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(proxy.OpenSession(handles), MemRpc::StatusCode::Ok);
    const uint64_t session_id = handles.sessionId;

    VesHeartbeatReply reply{};
    EXPECT_EQ(proxy.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkIdle));
    EXPECT_EQ(reply.sessionId, session_id);

    proxy.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesHealthyReply) {
    const std::string socketPath = "/tmp/virus_executor_service_health_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VesControlProxy proxy(stub->AsObject(), socketPath);
    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(proxy.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto result = proxy.CheckHealth(handles.sessionId);
    EXPECT_EQ(result.status, MemRpc::ChannelHealthStatus::Healthy);
    EXPECT_EQ(result.sessionId, handles.sessionId);

    proxy.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, CheckHealthTranslatesUnhealthyAndSessionMismatchReplies) {
    const std::string socketPath =
        "/tmp/virus_executor_service_health_mismatch_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VesControlProxy proxy(stub->AsObject(), socketPath);

    const auto unhealthy = proxy.CheckHealth(42);
    EXPECT_EQ(unhealthy.status, MemRpc::ChannelHealthStatus::Unhealthy);
    EXPECT_EQ(unhealthy.sessionId, 0u);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(proxy.OpenSession(handles), MemRpc::StatusCode::Ok);

    const auto mismatch = proxy.CheckHealth(handles.sessionId + 1);
    EXPECT_EQ(mismatch.status, MemRpc::ChannelHealthStatus::SessionMismatch);
    EXPECT_EQ(mismatch.sessionId, handles.sessionId);

    proxy.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, HeartbeatShowsInFlight) {
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), MemRpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep50.bin";
        started.store(true);
        (void)service.service().ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_GE(reply.inFlight, 1u);
    EXPECT_STREQ(reply.currentTask, "active");
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_BUSY, 0u);

    worker.join();
    service.CloseSession();
    service.OnStop();
}

TEST(VesHeartbeatTest, LongRunningHeartbeatIsDegraded) {
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), MemRpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep200_long.bin";
        started.store(true);
        (void)service.service().ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));
    EXPECT_NE(reply.flags & VES_HEARTBEAT_FLAG_LONG_RUNNING, 0u);
    EXPECT_GE(reply.lastTaskAgeMs, 100u);

    worker.join();
    service.CloseSession();
    service.OnStop();
}

}  // namespace VirusExecutorService
