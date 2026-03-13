#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "vpsdemo/ves/ves_engine_service.h"
#include "vpsdemo/ves/ves_types.h"

namespace vpsdemo {

TEST(VesHealthTest, SnapshotIdleDefaults) {
    VesEngineService service;
    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.inFlight, 0u);
    EXPECT_EQ(snapshot.currentTask, "idle");
    EXPECT_EQ(snapshot.lastTaskAgeMs, 0u);
}

TEST(VesHealthTest, SnapshotUpdatesAfterScan) {
    VesEngineService service;
    service.Initialize();

    ScanFileRequest req;
    req.filePath = "/data/virus.apk";

    auto reply = service.ScanFile(req);
    EXPECT_EQ(reply.code, 0);

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.inFlight, 0u);
    EXPECT_EQ(snapshot.currentTask, "idle");
}

TEST(VesHealthTest, InFlightAndAgeDuringScan) {
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
    EXPECT_GE(snapshot.inFlight, 1u);
    EXPECT_NE(snapshot.currentTask, "idle");

    worker.join();
}

}  // namespace vpsdemo
