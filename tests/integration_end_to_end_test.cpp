#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "memrpc/client.h"
#include "memrpc/demo_bootstrap.h"
#include "memrpc/server.h"

namespace {

class FakeHandler : public memrpc::IScanHandler {
 public:
  memrpc::ScanResult HandleScan(const memrpc::ScanRequest& request) override {
    memrpc::ScanResult result;
    result.status = memrpc::StatusCode::kOk;
    result.verdict = request.file_path.find("virus") != std::string::npos
                         ? memrpc::ScanVerdict::kInfected
                         : memrpc::ScanVerdict::kClean;
    result.message = request.file_path;
    return result;
  }
};

class SlowHandler : public memrpc::IScanHandler {
 public:
  memrpc::ScanResult HandleScan(const memrpc::ScanRequest& request) override {
    if (request.file_path.find("very-slow") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    } else if (request.file_path.find("slow") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    memrpc::ScanResult result;
    result.status = memrpc::StatusCode::kOk;
    result.verdict = memrpc::ScanVerdict::kClean;
    result.message = request.file_path;
    return result;
  }
};

}  // namespace

TEST(IntegrationEndToEndTest, ScanRoundTripOverSharedMemory) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::kOk);

  auto handler = std::make_shared<FakeHandler>();
  memrpc::EngineServer server(bootstrap->server_handles(), handler);
  ASSERT_EQ(server.Start(), memrpc::StatusCode::kOk);

  memrpc::EngineClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::kOk);

  memrpc::ScanRequest request;
  request.file_path = "/tmp/clean-file";
  memrpc::ScanResult result;
  EXPECT_EQ(client.Scan(request, &result), memrpc::StatusCode::kOk);
  EXPECT_EQ(result.verdict, memrpc::ScanVerdict::kClean);
  EXPECT_EQ(result.message, request.file_path);

  client.Shutdown();
  server.Stop();
}

TEST(IntegrationEndToEndTest, HighPriorityRequestBypassesSlowNormalWork) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::kOk);

  auto handler = std::make_shared<SlowHandler>();
  memrpc::EngineServer server(bootstrap->server_handles(), handler, {.high_worker_threads = 1, .normal_worker_threads = 1});
  ASSERT_EQ(server.Start(), memrpc::StatusCode::kOk);

  memrpc::EngineClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::kOk);

  memrpc::ScanRequest slow_request;
  slow_request.file_path = "/tmp/slow-normal";
  slow_request.options.priority = memrpc::Priority::kNormal;

  memrpc::ScanResult slow_result;
  std::thread slow_thread([&] {
    EXPECT_EQ(client.Scan(slow_request, &slow_result), memrpc::StatusCode::kOk);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  memrpc::ScanRequest high_request;
  high_request.file_path = "/tmp/high-fast";
  high_request.options.priority = memrpc::Priority::kHigh;

  const auto start = std::chrono::steady_clock::now();
  memrpc::ScanResult high_result;
  EXPECT_EQ(client.Scan(high_request, &high_result), memrpc::StatusCode::kOk);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  slow_thread.join();
  EXPECT_LT(elapsed, 150);
  EXPECT_EQ(high_result.message, high_request.file_path);
  EXPECT_EQ(slow_result.message, slow_request.file_path);

  client.Shutdown();
  server.Stop();
}

TEST(IntegrationEndToEndTest, ExecutionTimeoutMapsToExplicitError) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::kOk);

  auto handler = std::make_shared<SlowHandler>();
  memrpc::EngineServer server(bootstrap->server_handles(), handler);
  ASSERT_EQ(server.Start(), memrpc::StatusCode::kOk);

  memrpc::EngineClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::kOk);

  memrpc::ScanRequest request;
  request.file_path = "/tmp/slow-timeout";
  request.options.exec_timeout_ms = 50;

  memrpc::ScanResult result;
  EXPECT_EQ(client.Scan(request, &result), memrpc::StatusCode::kExecTimeout);
  EXPECT_EQ(result.status, memrpc::StatusCode::kExecTimeout);

  client.Shutdown();
  server.Stop();
}

TEST(IntegrationEndToEndTest, TimedOutRequestDoesNotReleaseSlotUntilLateResponseArrives) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>(
      memrpc::DemoBootstrapConfig{.high_ring_size = 8, .normal_ring_size = 8, .response_ring_size = 8, .slot_count = 1});
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::kOk);

  auto handler = std::make_shared<SlowHandler>();
  memrpc::EngineServer server(bootstrap->server_handles(), handler);
  ASSERT_EQ(server.Start(), memrpc::StatusCode::kOk);

  memrpc::EngineClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::kOk);

  memrpc::ScanRequest slow_request;
  slow_request.file_path = "/tmp/very-slow-timeout";
  slow_request.options.queue_timeout_ms = 1;
  slow_request.options.exec_timeout_ms = 10;
  memrpc::ScanResult slow_result;
  EXPECT_EQ(client.Scan(slow_request, &slow_result), memrpc::StatusCode::kPeerDisconnected);

  memrpc::ScanRequest blocked_request;
  blocked_request.file_path = "/tmp/after-timeout";
  memrpc::ScanResult blocked_result;
  EXPECT_EQ(client.Scan(blocked_request, &blocked_result), memrpc::StatusCode::kQueueFull);

  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  memrpc::ScanResult recovered_result;
  EXPECT_EQ(client.Scan(blocked_request, &recovered_result), memrpc::StatusCode::kOk);
  EXPECT_EQ(recovered_result.message, blocked_request.file_path);

  client.Shutdown();
  server.Stop();
}

TEST(IntegrationEndToEndTest, ConcurrentScansSucceed) {
  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::kOk);

  auto handler = std::make_shared<FakeHandler>();
  memrpc::EngineServer server(bootstrap->server_handles(), handler, {.high_worker_threads = 2, .normal_worker_threads = 2});
  ASSERT_EQ(server.Start(), memrpc::StatusCode::kOk);

  memrpc::EngineClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::kOk);

  constexpr int kThreadCount = 8;
  std::vector<std::thread> threads;
  std::vector<memrpc::ScanResult> results(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([i, &client, &results] {
      memrpc::ScanRequest request;
      request.file_path = "/tmp/file-" + std::to_string(i);
      EXPECT_EQ(client.Scan(request, &results[i]), memrpc::StatusCode::kOk);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (int i = 0; i < kThreadCount; ++i) {
    EXPECT_EQ(results[i].message, "/tmp/file-" + std::to_string(i));
  }

  client.Shutdown();
  server.Stop();
}
