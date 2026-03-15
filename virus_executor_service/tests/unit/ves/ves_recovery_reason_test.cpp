#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "service/virus_executor_service.h"
#include "transport/ves_control_interface.h"

namespace VirusExecutorService {

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
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), MemRpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep50_reason.bin";
        started.store(true);
        (void)service.service().ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    VesHeartbeatReply reply{};
    ASSERT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));

    worker.join();
    service.CloseSession();
    service.OnStop();
}

TEST(VesRecoveryReasonTest, LongRunningMapsToLongRunningReason) {
    VirusExecutorService service;
    service.OnStart();

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), MemRpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep200_reason.bin";
        started.store(true);
        (void)service.service().ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    VesHeartbeatReply reply{};
    ASSERT_EQ(service.Heartbeat(reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(reply.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));

    worker.join();
    service.CloseSession();
    service.OnStop();
}

}  // namespace VirusExecutorService
