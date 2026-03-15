#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ves/ves_engine_service.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

TEST(VesHealthTest, SnapshotBeforeInitializeIsInternalError) {
    VesEngineService service;
    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.status,
              static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyInternalError));
    EXPECT_EQ(snapshot.reasonCode,
              static_cast<uint32_t>(VesHeartbeatReasonCode::InternalError));
    EXPECT_EQ(snapshot.flags, 0u);
    EXPECT_EQ(snapshot.inFlight, 0u);
    EXPECT_EQ(snapshot.currentTask, "idle");
    EXPECT_EQ(snapshot.lastTaskAgeMs, 0u);
}

TEST(VesHealthTest, SnapshotUpdatesAfterScan) {
    VesEngineService service;
    service.Initialize();

    auto idleSnapshot = service.GetHealthSnapshot();
    EXPECT_EQ(idleSnapshot.status, static_cast<uint32_t>(VesHeartbeatStatus::OkIdle));
    EXPECT_EQ(idleSnapshot.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::None));
    EXPECT_EQ(idleSnapshot.flags, VES_HEARTBEAT_FLAG_INITIALIZED);

    ScanTask req;
    req.path = "/data/virus.apk";

    auto reply = service.ScanFile(req);
    EXPECT_EQ(reply.code, 0);

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.status, static_cast<uint32_t>(VesHeartbeatStatus::OkIdle));
    EXPECT_EQ(snapshot.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::None));
    EXPECT_EQ(snapshot.flags, VES_HEARTBEAT_FLAG_INITIALIZED);
    EXPECT_EQ(snapshot.inFlight, 0u);
    EXPECT_EQ(snapshot.currentTask, "idle");
}

TEST(VesHealthTest, InFlightAndAgeDuringScan) {
    VesEngineService service;
    service.Initialize();

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep50.bin";
        started.store(true);
        (void)service.ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(snapshot.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));
    EXPECT_EQ(snapshot.flags,
              VES_HEARTBEAT_FLAG_INITIALIZED | VES_HEARTBEAT_FLAG_BUSY);
    EXPECT_GE(snapshot.inFlight, 1u);
    EXPECT_NE(snapshot.currentTask, "idle");

    worker.join();
}

TEST(VesHealthTest, SnapshotTracksOldestInFlightTaskUnderConcurrency) {
    VesEngineService service;
    service.Initialize();

    std::atomic<bool> longStarted{false};
    std::atomic<bool> shortStarted{false};

    std::thread longWorker([&]() {
        ScanTask req;
        req.path = "/data/sleep200_long.bin";
        longStarted.store(true);
        (void)service.ScanFile(req);
    });

    while (!longStarted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    while (true) {
        auto snapshot = service.GetHealthSnapshot();
        if (snapshot.inFlight >= 1u &&
            snapshot.currentTask == "/data/sleep200_long.bin") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::thread shortWorker([&]() {
        ScanTask req;
        req.path = "/data/sleep50_short.bin";
        shortStarted.store(true);
        (void)service.ScanFile(req);
    });

    while (!shortStarted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    VesHealthSnapshot snapshot;
    while (true) {
        snapshot = service.GetHealthSnapshot();
        if (snapshot.inFlight >= 2u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(snapshot.currentTask, "/data/sleep200_long.bin");
    EXPECT_GE(snapshot.lastTaskAgeMs, 20u);
    EXPECT_EQ(snapshot.status, static_cast<uint32_t>(VesHeartbeatStatus::OkBusy));
    EXPECT_EQ(snapshot.reasonCode, static_cast<uint32_t>(VesHeartbeatReasonCode::Busy));

    shortWorker.join();
    longWorker.join();
}

TEST(VesHealthTest, SnapshotMarksLongRunningTaskAge) {
    VesEngineService service;
    service.Initialize();

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanTask req;
        req.path = "/data/sleep200_threshold.bin";
        started.store(true);
        (void)service.ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(
        VesEngineService::LONG_RUNNING_TASK_THRESHOLD_MS + 20));

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.status, static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning));
    EXPECT_EQ(snapshot.reasonCode,
              static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning));
    EXPECT_EQ(snapshot.flags,
              VES_HEARTBEAT_FLAG_INITIALIZED | VES_HEARTBEAT_FLAG_BUSY |
                  VES_HEARTBEAT_FLAG_LONG_RUNNING);
    EXPECT_GE(snapshot.lastTaskAgeMs, VesEngineService::LONG_RUNNING_TASK_THRESHOLD_MS);

    worker.join();
}

}  // namespace VirusExecutorService
