#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/common/minirpc_codec.h"
#include "apps/minirpc/minirpc_stress_config.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/server/rpc_server.h"
#include "virus_protection_service_log.h"

#include "apps/minirpc/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

namespace Mem = ::memrpc;

uint64_t MonotonicNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void CloseHandles(Mem::BootstrapHandles& h) {
  if (h.shm_fd >= 0) close(h.shm_fd);
  if (h.high_req_event_fd >= 0) close(h.high_req_event_fd);
  if (h.normal_req_event_fd >= 0) close(h.normal_req_event_fd);
  if (h.resp_event_fd >= 0) close(h.resp_event_fd);
  if (h.req_credit_event_fd >= 0) close(h.req_credit_event_fd);
  if (h.resp_credit_event_fd >= 0) close(h.resp_credit_event_fd);
}

struct SharedState {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> okCount{0};
  std::atomic<uint64_t> failCount{0};
  std::atomic<uint64_t> lastOkMs{0};
  std::mutex errorMutex;
  std::string error;
};

void WritePidFile(pid_t childPid) {
  const char* path = std::getenv("MEMRPC_STRESS_PID_FILE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output.is_open()) {
    return;
  }
  output << "parent_pid=" << static_cast<long long>(getpid()) << "\n";
  output << "child_pid=" << static_cast<long long>(childPid) << "\n";
}

void RunChild(Mem::BootstrapHandles handles, uint32_t threads) {
  Mem::RpcServer server;
  server.SetBootstrapHandles(handles);
  Mem::ServerOptions options;
  options.high_worker_threads = threads;
  options.normal_worker_threads = threads;
  server.SetOptions(options);

  MiniRpcService service;
  service.RegisterHandlers(&server);
  if (server.Start() != Mem::StatusCode::Ok) {
    HLOGE("minirpc stress server start failed");
    std::_Exit(1);
  }
  server.Run();
}

enum class RpcKind { Echo, Add, Sleep };

RpcKind PickKind(std::mt19937_64& rng, const StressConfig& config) {
  const int total = std::max(0, config.echoWeight) +
                    std::max(0, config.addWeight) +
                    std::max(0, config.sleepWeight);
  if (total <= 0) {
    return RpcKind::Echo;
  }
  std::uniform_int_distribution<int> dist(1, total);
  const int pick = dist(rng);
  if (pick <= config.echoWeight) {
    return RpcKind::Echo;
  }
  if (pick <= config.echoWeight + config.addWeight) {
    return RpcKind::Add;
  }
  return RpcKind::Sleep;
}

std::size_t PickPayloadSize(std::mt19937_64& rng, const StressConfig& config) {
  if (config.payloadSizes.empty()) {
    return 0;
  }
  std::uniform_int_distribution<std::size_t> dist(0, config.payloadSizes.size() - 1);
  return config.payloadSizes[dist(rng)];
}

Mem::Priority PickPriority(std::mt19937_64& rng, const StressConfig& config) {
  const int pct = std::max(0, std::min(100, config.highPriorityPct));
  std::uniform_int_distribution<int> dist(1, 100);
  return dist(rng) <= pct ? Mem::Priority::High : Mem::Priority::Normal;
}

bool InBurstWindow(uint64_t elapsedMs, const StressConfig& config) {
  if (config.burstIntervalMs <= 0 || config.burstDurationMs <= 0) {
    return false;
  }
  const uint64_t interval = static_cast<uint64_t>(config.burstIntervalMs);
  const uint64_t duration = static_cast<uint64_t>(config.burstDurationMs);
  return (elapsedMs % interval) < duration;
}

void RecordError(SharedState* state, const std::string& message) {
  if (state == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->errorMutex);
  if (state->error.empty()) {
    state->error = message;
  }
  state->stop.store(true);
}

bool RunStress(const StressConfig& config) {
  Mem::DemoBootstrapConfig bootstrapConfig;
  bootstrapConfig.max_request_bytes = config.maxRequestBytes;
  bootstrapConfig.max_response_bytes = config.maxResponseBytes;

  auto bootstrap = std::make_shared<Mem::PosixDemoBootstrapChannel>(bootstrapConfig);
  Mem::BootstrapHandles unusedHandles;
  if (bootstrap->OpenSession(&unusedHandles) != Mem::StatusCode::Ok) {
    HLOGE("stress bootstrap open session failed");
    return false;
  }
  CloseHandles(unusedHandles);

  const Mem::BootstrapHandles serverHandles = bootstrap->serverHandles();
  const pid_t child = fork();
  if (child == 0) {
    RunChild(serverHandles, static_cast<uint32_t>(config.threads));
    return true;
  }
  if (child < 0) {
    HLOGE("stress fork failed");
    return false;
  }

  WritePidFile(child);

  Mem::RpcClient client(bootstrap);
  if (client.Init() != Mem::StatusCode::Ok) {
    HLOGE("stress client init failed");
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    return false;
  }

  SharedState state;
  state.lastOkMs.store(MonotonicNowMs());

  const auto start = std::chrono::steady_clock::now();
  const auto warmupEnd = start + std::chrono::seconds(config.warmupSec);
  const auto end = warmupEnd + std::chrono::seconds(config.durationSec);

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.threads));

  for (int i = 0; i < config.threads; ++i) {
    workers.emplace_back([&, i]() {
      std::mt19937_64 rng(config.seed ? config.seed + i : (MonotonicNowMs() + i));
      while (!state.stop.load() && std::chrono::steady_clock::now() < end) {
        const uint64_t elapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        const RpcKind kind = PickKind(rng, config);
        const Mem::Priority priority = PickPriority(rng, config);

        Mem::StatusCode status = Mem::StatusCode::Ok;
        if (kind == RpcKind::Echo) {
          const std::size_t size = PickPayloadSize(rng, config);
          std::string text(size, 'x');
          EchoRequest request{text};
          EchoReply reply;
          status = Mem::InvokeTypedSync(&client, static_cast<Mem::Opcode>(MiniRpcOpcode::MiniEcho), request, &reply, priority);
        } else if (kind == RpcKind::Add) {
          AddRequest request{static_cast<int32_t>(rng()), static_cast<int32_t>(rng())};
          AddReply reply;
          status = Mem::InvokeTypedSync(&client, static_cast<Mem::Opcode>(MiniRpcOpcode::MiniAdd), request, &reply, priority);
        } else {
          const uint32_t delayMs = static_cast<uint32_t>(rng() % std::max(1, config.maxSleepMs));
          SleepRequest request{delayMs};
          SleepReply reply;
          status = Mem::InvokeTypedSync(&client, static_cast<Mem::Opcode>(MiniRpcOpcode::MiniSleep), request, &reply, priority);
        }

        if (status != Mem::StatusCode::Ok) {
          state.failCount.fetch_add(1);
          RecordError(&state, "rpc failed status=" + std::to_string(static_cast<int>(status)));
          break;
        }

        if (std::chrono::steady_clock::now() >= warmupEnd) {
          state.okCount.fetch_add(1);
          state.lastOkMs.store(MonotonicNowMs());
        }

        if (!InBurstWindow(elapsedMs, config)) {
          const int sleepUs = static_cast<int>((rng() % 1000) * config.burstMultiplier);
          if (sleepUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
          }
        }
      }
    });
  }

  std::thread monitor([&]() {
    while (!state.stop.load() && std::chrono::steady_clock::now() < end) {
      const uint64_t nowMs = MonotonicNowMs();
      const uint64_t lastOk = state.lastOkMs.load();
      if (nowMs > lastOk && (nowMs - lastOk) / 1000 > static_cast<uint64_t>(config.noProgressTimeoutSec)) {
        RecordError(&state, "no progress within timeout");
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  for (auto& worker : workers) {
    worker.join();
  }
  state.stop.store(true);
  monitor.join();

  client.Shutdown();
  kill(child, SIGTERM);
  waitpid(child, nullptr, 0);

  if (!state.error.empty()) {
    HLOGE("stress failed: %s", state.error.c_str());
    return false;
  }

  HLOGI("stress ok: ops=%{public}llu", static_cast<unsigned long long>(state.okCount.load()));
  return true;
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

int main() {
  using namespace OHOS::Security::VirusProtectionService::MiniRpc;
  const StressConfig config = ParseStressConfigFromEnv();
  HLOGI("stress start: duration=%{public}d warmup=%{public}d threads=%{public}d",
        config.durationSec, config.warmupSec, config.threads);
  const bool ok = RunStress(config);
  return ok ? 0 : 1;
}
