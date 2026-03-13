#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ves_engine_service.h"
#include "ves_types.h"

namespace vpsdemo {

TEST(VpsHealthTest, SnapshotIdleDefaults) {
    VesEngineService service;
    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.in_flight, 0u);
    EXPECT_EQ(snapshot.current_task, "idle");
    EXPECT_EQ(snapshot.last_task_age_ms, 0u);
}

TEST(VpsHealthTest, SnapshotUpdatesAfterScan) {
    VesEngineService service;
    service.Initialize();

    ScanFileRequest req;
    req.filePath = "/data/virus.apk";

    auto reply = service.ScanFile(req);
    EXPECT_EQ(reply.code, 0);

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.in_flight, 0u);
    EXPECT_EQ(snapshot.current_task, "idle");
}

TEST(VpsHealthTest, InFlightAndAgeDuringScan) {
    VesEngineService service;
    service.Initialize();

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanFileRequest req;
        req.filePath = "/data/sleep50.bin";
        started.store(true);
        (void)service.ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_GE(snapshot.in_flight, 1u);
    EXPECT_NE(snapshot.current_task, "idle");

    worker.join();
}

}  // namespace vpsdemo
