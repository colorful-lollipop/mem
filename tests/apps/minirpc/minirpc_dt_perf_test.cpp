#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

#include "core/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles) {
  if (handles.shmFd >= 0) close(handles.shmFd);
  if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
  if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
  if (handles.respEventFd >= 0) close(handles.respEventFd);
  if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
  if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
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

uint32_t GetThreadCount() {
  const unsigned int hw = std::thread::hardware_concurrency();
  const unsigned int hw_threads = hw == 0 ? 1u : hw;
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw_threads)));
  return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", default_threads));
}

std::filesystem::path BaselinePath() {
  const char* dir = std::getenv("MEMRPC_DT_BASELINE_DIR");
  std::filesystem::path base = (dir && *dir) ? dir : (std::filesystem::current_path() / "perf_baselines");
  return base / "minirpc_dt_perf.baseline";
}

std::map<std::string, double> LoadBaseline(const std::filesystem::path& path) {
  std::map<std::string, double> baseline;
  std::ifstream input(path);
  if (!input.is_open()) {
    return baseline;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    const std::string value_str = line.substr(eq + 1);
    try {
      baseline[key] = std::stod(value_str);
    } catch (const std::exception&) {
      continue;
    }
  }
  return baseline;
}

bool WriteBaseline(const std::filesystem::path& path,
                   const std::map<std::string, double>& baseline) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output << "# minirpc dt perf baseline\n";
  for (const auto& entry : baseline) {
    output << entry.first << "=" << std::fixed << std::setprecision(3) << entry.second << "\n";
  }
  return true;
}

struct PerfStats {
  double ops_per_sec = 0.0;
  double p99_us = 0.0;
};

PerfStats ComputeStats(const std::vector<double>& latencies_us, double duration_sec, uint64_t ops) {
  PerfStats stats;
  stats.ops_per_sec = duration_sec > 0.0 ? ops / duration_sec : 0.0;
  if (!latencies_us.empty()) {
    std::vector<double> sorted = latencies_us;
    std::sort(sorted.begin(), sorted.end());
    stats.p99_us = sorted[sorted.size() * 99 / 100];
  }
  return stats;
}

}  // namespace

TEST(MiniRpcDtPerfTest, ShortPerfBaseline) {
  const int durationMs = GetEnvInt("MEMRPC_DT_durationMs", 3000);
  const int warmup_ms = GetEnvInt("MEMRPC_DT_WARMUP_MS", 200);
  const int min_ops = GetEnvInt("MEMRPC_DT_MIN_OPS", 50);
  const int max_p99_us = GetEnvInt("MEMRPC_DT_MAX_P99_US", 20000);
  const uint32_t threadCount = GetThreadCount();

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  MemRpc::ServerOptions options;
  options.highWorkerThreads = threadCount;
  options.normalWorkerThreads = threadCount;
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  const size_t max_echo_payload =
      MemRpc::DEFAULT_MAX_REQUEST_BYTES > 4 ? MemRpc::DEFAULT_MAX_REQUEST_BYTES - 4 : 0;
  const size_t echo_large_payload = std::min<size_t>(4096, max_echo_payload);
  const std::vector<std::pair<const char*, size_t>> cases = {
      {"echo_0B", 0},
      {"echo_4KB", echo_large_payload},
      {"add", 0},
  };

  std::map<std::string, double> baseline = LoadBaseline(BaselinePath());
  bool baseline_updated = false;

  for (const auto& c : cases) {
    const std::string case_name = c.first;
    const size_t payload_size = c.second;

    const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
    while (std::chrono::steady_clock::now() < warmup_end) {
      if (case_name == "add") {
        AddReply reply;
        (void)client.Add(1, 2, &reply);
      } else {
        EchoReply reply;
        std::string payload(payload_size, 'x');
        (void)client.Echo(payload, &reply);
      }
    }

    std::atomic<uint64_t> ops{0};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::milliseconds(durationMs);

    for (uint32_t i = 0; i < threadCount; ++i) {
      workers.emplace_back([&, i]() {
        while (std::chrono::steady_clock::now() < end) {
          MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
          if (case_name == "add") {
            AddReply reply;
            status = client.Add(1, 2, &reply);
          } else {
            EchoReply reply;
            std::string payload(payload_size, static_cast<char>('a' + (i % 8)));
            status = client.Echo(payload, &reply);
          }
          if (status == MemRpc::StatusCode::Ok) {
            ++ops;
          }
        }
      });
    }

    for (auto& t : workers) {
      t.join();
    }

    std::vector<double> latencies;
    latencies.reserve(500);
    for (int i = 0; i < 500; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
      if (case_name == "add") {
        AddReply reply;
        status = client.Add(1, 2, &reply);
      } else {
        EchoReply reply;
        std::string payload(payload_size, 'x');
        status = client.Echo(payload, &reply);
      }
      const auto t1 = std::chrono::steady_clock::now();
      ASSERT_EQ(status, MemRpc::StatusCode::Ok);
      const double us = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
      latencies.push_back(us);
    }

    const double duration_sec = std::max(1, durationMs) / 1000.0;
    const PerfStats stats = ComputeStats(latencies, duration_sec, ops.load());

    const std::string ops_key = "minirpc." + case_name + ".threads=" + std::to_string(threadCount) + ".ops_per_sec";
    const std::string p99_key = "minirpc." + case_name + ".threads=" + std::to_string(threadCount) + ".p99_us";

    EXPECT_GE(stats.ops_per_sec, static_cast<double>(min_ops));
    EXPECT_LE(stats.p99_us, static_cast<double>(max_p99_us));

    const double ops_baseline = baseline.count(ops_key) ? baseline[ops_key] : 0.0;
    const double p99_baseline = baseline.count(p99_key) ? baseline[p99_key] : 0.0;

    if (ops_baseline > 0.0 && stats.ops_per_sec < ops_baseline * 0.9) {
      ADD_FAILURE() << "Throughput regression for " << ops_key << ": baseline=" << ops_baseline
                    << " current=" << stats.ops_per_sec;
    }
    if (p99_baseline > 0.0 && stats.p99_us > p99_baseline * 1.1) {
      ADD_FAILURE() << "Latency regression for " << p99_key << ": baseline=" << p99_baseline
                    << " current=" << stats.p99_us;
    }

    if (stats.ops_per_sec > ops_baseline) {
      baseline[ops_key] = stats.ops_per_sec;
      baseline_updated = true;
    }
    if (p99_baseline <= 0.0 || stats.p99_us < p99_baseline) {
      baseline[p99_key] = stats.p99_us;
      baseline_updated = true;
    }
  }

  if (baseline_updated) {
    WriteBaseline(BaselinePath(), baseline);
  }

  client.Shutdown();
  server.Stop();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
