#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <unistd.h>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_async_client.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shm_fd >= 0) close(h.shm_fd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

int GetEnvInt(const char* name, int default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return default_value;
  }
  try {
    const int parsed = std::stoi(value);
    return parsed > 0 ? parsed : default_value;
  } catch (const std::exception&) {
    return default_value;
  }
}

struct PipelineResult {
  int batch_size = 0;
  double ops_per_sec = 0;
  uint64_t total_ops = 0;
};

TEST(MiniRpcAsyncPipelineTest, BatchSizeThroughput) {
  const int duration_ms = GetEnvInt("MEMRPC_PERF_DURATION_MS", 1000);
  const int warmup_ms = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  MemRpc::ServerOptions options;
  options.high_worker_threads = 4;
  options.normal_worker_threads = 4;
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  // Measure sync baseline first (using the same bootstrap).
  double sync_ops_per_sec = 0;
  {
    MiniRpcClient sync_client(bootstrap);
    ASSERT_EQ(sync_client.Init(), MemRpc::StatusCode::Ok);

    // Warmup.
    {
      const auto warmup_end =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
      while (std::chrono::steady_clock::now() < warmup_end) {
        EchoReply reply;
        sync_client.Echo("ping", &reply);
      }
    }

    uint64_t sync_ops = 0;
    const auto end_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < end_time) {
      EchoReply reply;
      MemRpc::StatusCode status = sync_client.Echo("ping", &reply);
      ASSERT_EQ(status, MemRpc::StatusCode::Ok);
      ++sync_ops;
    }
    const double duration_sec = std::max(1, duration_ms) / 1000.0;
    sync_ops_per_sec = sync_ops / duration_sec;
    sync_client.Shutdown();
  }

  // Async pipeline measurements.
  MiniRpcAsyncClient async_client(bootstrap);
  ASSERT_EQ(async_client.Init(), MemRpc::StatusCode::Ok);

  const std::vector<int> batch_sizes = {1, 8, 32, 64};
  std::vector<PipelineResult> results;

  std::cout << "\n=== Async Pipeline Throughput ===" << std::endl;

  for (const int batch_size : batch_sizes) {
    // Warmup phase.
    {
      const auto warmup_end =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
      while (std::chrono::steady_clock::now() < warmup_end) {
        EchoRequest req;
        req.text = "ping";
        std::vector<MemRpc::TypedFuture<EchoReply>> futures;
        futures.reserve(batch_size);
        for (int j = 0; j < batch_size; ++j) {
          futures.push_back(async_client.EchoAsync(req));
        }
        for (auto& f : futures) {
          EchoReply reply;
          f.Wait(&reply);
        }
      }
    }

    // Measurement phase.
    uint64_t total_ops = 0;
    const auto end_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < end_time) {
      EchoRequest req;
      req.text = "ping";
      std::vector<MemRpc::TypedFuture<EchoReply>> futures;
      futures.reserve(batch_size);
      for (int j = 0; j < batch_size; ++j) {
        futures.push_back(async_client.EchoAsync(req));
      }
      bool all_ok = true;
      for (auto& f : futures) {
        EchoReply reply;
        MemRpc::StatusCode status = f.Wait(&reply);
        if (status != MemRpc::StatusCode::Ok) {
          all_ok = false;
        }
      }
      if (!all_ok) {
        ADD_FAILURE() << "async batch failed for batch_size=" << batch_size;
        break;
      }
      total_ops += batch_size;
    }

    const double duration_sec = std::max(1, duration_ms) / 1000.0;
    PipelineResult result;
    result.batch_size = batch_size;
    result.total_ops = total_ops;
    result.ops_per_sec = total_ops / duration_sec;
    results.push_back(result);

    std::cout << "  batch=" << std::setw(3) << batch_size << ": " << std::fixed
              << std::setprecision(0) << result.ops_per_sec << " ops/sec" << std::endl;
  }

  std::cout << "  sync:     " << std::fixed << std::setprecision(0) << sync_ops_per_sec
            << " ops/sec" << std::endl;

  async_client.Shutdown();
  server.Stop();

  EXPECT_FALSE(results.empty());
  // Async pipeline should generally outperform sync for batch > 1.
  if (results.size() >= 2) {
    EXPECT_GT(results.back().ops_per_sec, results.front().ops_per_sec * 0.8);
  }
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
