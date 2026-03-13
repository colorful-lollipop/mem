#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
}

struct PerfConfig {
  int threads = 1;
  int durationMs = 1000;
  int warmup_ms = 200;
  std::filesystem::path baseline_path;
};

struct PerfCaseResult {
  std::string key;
  double ops_per_sec = 0.0;
  std::string error;
};

enum class RpcKind { Echo, Add, Sleep };

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

std::filesystem::path GetBaselinePath() {
  const char* path = std::getenv("MEMRPC_PERF_BASELINE_PATH");
  if (path != nullptr && *path != '\0') {
    return std::filesystem::path(path);
  }
  return std::filesystem::current_path() / "perf_baselines" / "minirpc_throughput.baseline";
}

PerfConfig LoadPerfConfig() {
  const unsigned int hw_threads = std::thread::hardware_concurrency();
  const unsigned int normalized_hw = hw_threads == 0 ? 1u : hw_threads;
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, normalized_hw)));
  PerfConfig config;
  config.threads = GetEnvInt("MEMRPC_PERF_THREADS", default_threads);
  config.durationMs = GetEnvInt("MEMRPC_PERF_durationMs", 1000);
  config.warmup_ms = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);
  config.baseline_path = GetBaselinePath();
  return config;
}

const char* RpcKindName(RpcKind kind) {
  switch (kind) {
    case RpcKind::Echo:
      return "echo";
    case RpcKind::Add:
      return "add";
    case RpcKind::Sleep:
      return "sleep";
  }
  return "unknown";
}

std::string MakeBaselineKey(RpcKind kind, int threads) {
  std::ostringstream oss;
  oss << RpcKindName(kind) << ".threads=" << threads;
  return oss.str();
}

bool InvokeOnce(MiniRpcClient* client,
                RpcKind kind,
                const std::string& echo_text,
                EchoReply* echo_reply,
                AddReply* add_reply,
                SleepReply* sleep_reply,
                MemRpc::StatusCode* status) {
  if (client == nullptr) {
    if (status != nullptr) {
      *status = MemRpc::StatusCode::InvalidArgument;
    }
    return false;
  }
  MemRpc::StatusCode call_status = MemRpc::StatusCode::InvalidArgument;
  switch (kind) {
    case RpcKind::Echo:
      call_status = client->Echo(echo_text, echo_reply);
      break;
    case RpcKind::Add:
      call_status = client->Add(1, 2, add_reply);
      break;
    case RpcKind::Sleep:
      call_status = client->Sleep(0, sleep_reply);
      break;
  }
  if (status != nullptr) {
    *status = call_status;
  }
  return call_status == MemRpc::StatusCode::Ok;
}

struct WorkerResult {
  uint64_t ops = 0;
  bool ok = true;
  MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
};

bool MeasureOpsPerSecond(const PerfConfig& config,
                         RpcKind kind,
                         const std::shared_ptr<MemRpc::IBootstrapChannel>& bootstrap,
                         double* ops_per_sec,
                         std::string* error) {
  if (ops_per_sec == nullptr) {
    if (error != nullptr) {
      *error = "missing ops_per_sec output";
    }
    return false;
  }

  const int threadCount = std::max(1, config.threads);
  MiniRpcClient client(bootstrap);
  const MemRpc::StatusCode init_status = client.Init();
  if (init_status != MemRpc::StatusCode::Ok) {
    if (error != nullptr) {
      std::ostringstream oss;
      oss << "client init failed status=" << static_cast<int>(init_status);
      *error = oss.str();
    }
    return false;
  }
  std::vector<WorkerResult> worker_results(threadCount);
  std::vector<std::thread> workers;
  workers.reserve(threadCount);

  const auto start_time =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  const auto warmup_end = start_time + std::chrono::milliseconds(config.warmup_ms);
  const auto end_time = warmup_end + std::chrono::milliseconds(config.durationMs);

  for (int i = 0; i < threadCount; ++i) {
    workers.emplace_back([&, i]() {
      WorkerResult& result = worker_results[i];

      while (std::chrono::steady_clock::now() < start_time) {
        std::this_thread::yield();
      }

      EchoReply echo_reply;
      AddReply add_reply;
      SleepReply sleep_reply;
      const std::string echo_text = "ping";

      while (std::chrono::steady_clock::now() < warmup_end) {
        if (!InvokeOnce(&client, kind, echo_text, &echo_reply, &add_reply, &sleep_reply,
                        &result.status)) {
          result.ok = false;
          break;
        }
      }

      while (result.ok && std::chrono::steady_clock::now() < end_time) {
        if (!InvokeOnce(&client, kind, echo_text, &echo_reply, &add_reply, &sleep_reply,
                        &result.status)) {
          result.ok = false;
          break;
        }
        ++result.ops;
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  client.Shutdown();

  for (const auto& result : worker_results) {
    if (!result.ok) {
      if (error != nullptr) {
        std::ostringstream oss;
        oss << "rpc failed status=" << static_cast<int>(result.status);
        *error = oss.str();
      }
      return false;
    }
  }

  const double duration_seconds = static_cast<double>(std::max(1, config.durationMs)) / 1000.0;
  uint64_t total_ops = 0;
  for (const auto& result : worker_results) {
    total_ops += result.ops;
  }
  *ops_per_sec = total_ops / duration_seconds;
  return true;
}

std::vector<PerfCaseResult> RunThroughputSuite(const PerfConfig& config) {
  std::vector<PerfCaseResult> results;
  const int threadCount = std::max(1, config.threads);
  const std::vector<RpcKind> kinds = {RpcKind::Echo, RpcKind::Add, RpcKind::Sleep};

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  {
    MemRpc::BootstrapHandles unused_handles;
    if (bootstrap->OpenSession(unused_handles) != MemRpc::StatusCode::Ok) {
      for (const auto kind : kinds) {
        results.push_back({MakeBaselineKey(kind, threadCount), 0.0,
                           "bootstrap start failed"});
      }
      return results;
    }
    CloseHandles(unused_handles);
  }

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  MemRpc::ServerOptions options;
  options.highWorkerThreads = static_cast<uint32_t>(threadCount);
  options.normalWorkerThreads = static_cast<uint32_t>(threadCount);
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  if (server.Start() != MemRpc::StatusCode::Ok) {
    for (const auto kind : kinds) {
      results.push_back(
          {MakeBaselineKey(kind, threadCount), 0.0, "server start failed"});
    }
    return results;
  }

  for (const auto kind : kinds) {
    PerfCaseResult result;
    result.key = MakeBaselineKey(kind, threadCount);
    if (!MeasureOpsPerSecond(config, kind, bootstrap, &result.ops_per_sec, &result.error)) {
      if (result.error.empty()) {
        result.error = "measurement failed";
      }
    }
    results.push_back(result);
  }

  server.Stop();

  return results;
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
  output << "# minirpc throughput baseline\n";
  for (const auto& entry : baseline) {
    output << entry.first << "=" << std::fixed << std::setprecision(3) << entry.second << "\n";
  }
  return true;
}

bool UpdateBaseline(const std::filesystem::path& path,
                    const std::vector<PerfCaseResult>& results,
                    std::map<std::string, double>* baseline) {
  if (baseline == nullptr) {
    return false;
  }
  bool updated = false;
  for (const auto& result : results) {
    if (!result.error.empty()) {
      ADD_FAILURE() << "Throughput measurement failed for " << result.key << ": "
                    << result.error;
      continue;
    }
    auto it = baseline->find(result.key);
    if (it == baseline->end()) {
      (*baseline)[result.key] = result.ops_per_sec;
      updated = true;
      continue;
    }

    const double baseline_value = it->second;
    if (baseline_value > 0.0 && result.ops_per_sec < baseline_value * 0.9) {
      ADD_FAILURE() << "Throughput regression for " << result.key << ": baseline="
                    << baseline_value << " current=" << result.ops_per_sec;
      continue;
    }

    if (result.ops_per_sec > baseline_value) {
      it->second = result.ops_per_sec;
      updated = true;
    }
  }

  if (updated) {
    WriteBaseline(path, *baseline);
  }
  return updated;
}

TEST(MiniRpcThroughputTest, RecordsAndValidatesBaseline) {
  const PerfConfig config = LoadPerfConfig();
  const std::vector<PerfCaseResult> results = RunThroughputSuite(config);
  std::map<std::string, double> baseline = LoadBaseline(config.baseline_path);
  const bool updated = UpdateBaseline(config.baseline_path, results, &baseline);

  EXPECT_FALSE(results.empty());
  EXPECT_TRUE(updated || !baseline.empty() ||
              std::filesystem::exists(config.baseline_path));
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
