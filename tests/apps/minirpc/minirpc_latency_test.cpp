#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
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

struct LatencyStats {
  double p50_us = 0;
  double p99_us = 0;
  double p999_us = 0;
  double max_us = 0;
  double mean_us = 0;
  uint64_t samples = 0;
};

LatencyStats ComputeStats(std::vector<double>& latencies_us) {
  LatencyStats stats;
  if (latencies_us.empty()) {
    return stats;
  }
  std::sort(latencies_us.begin(), latencies_us.end());
  stats.samples = latencies_us.size();
  stats.mean_us =
      std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) / stats.samples;
  stats.p50_us = latencies_us[stats.samples * 50 / 100];
  stats.p99_us = latencies_us[stats.samples * 99 / 100];
  stats.p999_us = latencies_us[std::min<size_t>(stats.samples - 1, stats.samples * 999 / 1000)];
  stats.max_us = latencies_us.back();
  return stats;
}

void PrintStats(const char* label, const LatencyStats& stats) {
  std::cout << std::setw(20) << label << ": "
            << "p50=" << std::fixed << std::setprecision(1) << stats.p50_us << "us  "
            << "p99=" << stats.p99_us << "us  "
            << "p999=" << stats.p999_us << "us  "
            << "max=" << stats.max_us << "us  "
            << "mean=" << stats.mean_us << "us  "
            << "n=" << stats.samples << std::endl;
}

TEST(MiniRpcLatencyTest, SingleThreadSerialLatencyByPayloadSize) {
  const int iterations = GetEnvInt("MEMRPC_LATENCY_ITERATIONS", 2000);
  const int warmup = GetEnvInt("MEMRPC_LATENCY_WARMUP", 200);

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  struct PayloadCase {
    const char* name;
    std::string text;
  };
  std::vector<PayloadCase> cases = {
      {"0B", ""},
      {"64B", std::string(64, 'x')},
      {"512B", std::string(512, 'x')},
      {"4KB", std::string(4000, 'x')},
  };

  std::cout << "\n=== RPC Latency Distribution (serial, single-thread) ===" << std::endl;

  for (const auto& pc : cases) {
    // Warm up.
    for (int i = 0; i < warmup; ++i) {
      EchoReply reply;
      MemRpc::StatusCode status = client.Echo(pc.text, &reply);
      ASSERT_EQ(status, MemRpc::StatusCode::Ok) << "warmup failed";
    }

    std::vector<double> latencies;
    latencies.reserve(iterations);
    for (int i = 0; i < iterations; ++i) {
      EchoReply reply;
      const auto start = std::chrono::steady_clock::now();
      MemRpc::StatusCode status = client.Echo(pc.text, &reply);
      const auto end = std::chrono::steady_clock::now();
      ASSERT_EQ(status, MemRpc::StatusCode::Ok);
      const double us =
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
      latencies.push_back(us);
    }
    LatencyStats stats = ComputeStats(latencies);
    PrintStats(pc.name, stats);
    EXPECT_GT(stats.samples, 0u);
  }

  client.Shutdown();
  server.Stop();
}

TEST(MiniRpcLatencyTest, DirectCallBaselineLatency) {
  const int iterations = GetEnvInt("MEMRPC_LATENCY_ITERATIONS", 2000);
  MiniRpcService service;

  std::cout << "\n=== Direct Call Latency (no RPC) ===" << std::endl;

  struct PayloadCase {
    const char* name;
    std::string text;
  };
  std::vector<PayloadCase> cases = {
      {"0B (direct)", ""},
      {"64B (direct)", std::string(64, 'x')},
      {"512B (direct)", std::string(512, 'x')},
      {"4KB (direct)", std::string(4000, 'x')},
  };

  for (const auto& pc : cases) {
    std::vector<double> latencies;
    latencies.reserve(iterations);
    EchoRequest req;
    req.text = pc.text;
    for (int i = 0; i < iterations; ++i) {
      const auto start = std::chrono::steady_clock::now();
      EchoReply reply = service.Echo(req);
      const auto end = std::chrono::steady_clock::now();
      (void)reply;
      const double us =
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
      latencies.push_back(us);
    }
    LatencyStats stats = ComputeStats(latencies);
    PrintStats(pc.name, stats);
    EXPECT_GT(stats.samples, 0u);
  }
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
