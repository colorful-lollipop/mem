#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "virus_executor_service/virus_executor_service.h"
#include "virus_executor_service/transport/ves_bootstrap_interface.h"
#include "virus_executor_service/transport/ves_bootstrap_proxy.h"
#include "virus_executor_service/ves/ves_types.h"

namespace virus_executor_service {

TEST(VesHeartbeatTest, UnhealthyBeforeOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::Unhealthy));

    service.OnStop();
}

TEST(VesHeartbeatTest, OkAfterOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), memrpc::StatusCode::Ok);
    const uint64_t session_id = handles.sessionId;

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::Ok));
    EXPECT_EQ(reply.sessionId, session_id);
    EXPECT_STREQ(reply.currentTask, "idle");
    EXPECT_EQ(reply.version, 1u);
    EXPECT_EQ(reply.inFlight, 0u);
    EXPECT_EQ(reply.lastTaskAgeMs, 0u);

    service.CloseSession();
    service.OnStop();
}

TEST(VesHeartbeatTest, HeartbeatOverSaSocket) {
    const std::string socketPath = "/tmp/virus_executor_service_hb_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VesBootstrapProxy proxy(stub->AsObject(), socketPath);
    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(proxy.OpenSession(handles), memrpc::StatusCode::Ok);
    const uint64_t session_id = handles.sessionId;

    VesHeartbeatReply reply{};
    EXPECT_EQ(proxy.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::Ok));
    EXPECT_EQ(reply.sessionId, session_id);

    proxy.CloseSession();
    stub->OnStop();
}

TEST(VesHeartbeatTest, HeartbeatShowsInFlight) {
    VirusExecutorService service;
    service.OnStart();

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), memrpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        virus_executor_service::ScanFileRequest req;
        req.filePath = "/data/sleep50.bin";
        started.store(true);
        (void)service.service().ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_GE(reply.inFlight, 1u);
    EXPECT_STRNE(reply.currentTask, "idle");

    worker.join();
    service.CloseSession();
    service.OnStop();
}

}  // namespace virus_executor_service
