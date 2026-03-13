# MemRPC + MiniRPC Stability Stress and Fuzz Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add isolated stress runner, sanitizer-friendly workflows, and libFuzzer targets for `minirpc` without impacting default `ctest` runs.

**Architecture:** A standalone stress runner binary forks a server and drives randomized client load with configurable mix/threads/duration. Fuzz targets live under `tests/fuzz` and are built only when `MEMRPC_ENABLE_FUZZ=ON`. Build toggles keep all stress/fuzz targets out of default builds and DT.

**Tech Stack:** CMake, GoogleTest, libFuzzer (Clang), sanitizers (ASan/UBSan/LSan/TSan), Bash.

---

### Task 1: Add Stress Config Parser + Unit Test (and CMake toggles)

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/apps/minirpc/CMakeLists.txt`
- Create: `tests/apps/minirpc/minirpc_stress_config.h`
- Create: `tests/apps/minirpc/minirpc_stress_config.cpp`
- Create: `tests/apps/minirpc/minirpc_stress_config_test.cpp`

**Step 1: Write the failing test**

Create `tests/apps/minirpc/minirpc_stress_config_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdlib>

#include "apps/minirpc/minirpc_stress_config.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

class ScopedEnv {
 public:
  ScopedEnv(const char* key, const char* value) : key_(key) {
    const char* prev = std::getenv(key);
    if (prev != nullptr) {
      had_prev_ = true;
      prev_value_ = prev;
    }
    setenv(key, value, 1);
  }

  ~ScopedEnv() {
    if (had_prev_) {
      setenv(key_, prev_value_.c_str(), 1);
    } else {
      unsetenv(key_);
    }
  }

 private:
  const char* key_;
  bool had_prev_ = false;
  std::string prev_value_;
};

}  // namespace

TEST(MiniRpcStressConfigTest, ParsesEnvOverrides) {
  ScopedEnv duration("MEMRPC_STRESS_DURATION_SEC", "5");
  ScopedEnv threads("MEMRPC_STRESS_THREADS", "3");
  ScopedEnv payloads("MEMRPC_STRESS_PAYLOAD_SIZES", "0,16,128");
  ScopedEnv highprio("MEMRPC_STRESS_HIGH_PRIORITY_PCT", "25");
  ScopedEnv burst("MEMRPC_STRESS_BURST_INTERVAL_MS", "500");

  const StressConfig config = ParseStressConfigFromEnv();

  EXPECT_EQ(config.durationSec, 5);
  EXPECT_EQ(config.threads, 3);
  ASSERT_EQ(config.payloadSizes.size(), 3u);
  EXPECT_EQ(config.payloadSizes[0], 0u);
  EXPECT_EQ(config.payloadSizes[1], 16u);
  EXPECT_EQ(config.payloadSizes[2], 128u);
  EXPECT_EQ(config.highPriorityPct, 25);
  EXPECT_EQ(config.burstIntervalMs, 500);
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
```

Update `tests/apps/minirpc/CMakeLists.txt` to add a gated stress library + test (place at end):

```cmake
if (MEMRPC_ENABLE_STRESS_TESTS)
  add_library(minirpc_stress_config
    minirpc_stress_config.cpp
  )
  target_link_libraries(minirpc_stress_config PRIVATE minirpc_demo)
  target_compile_features(minirpc_stress_config PRIVATE cxx_std_17)

  add_executable(memrpc_minirpc_stress_config_test minirpc_stress_config_test.cpp)
  target_link_libraries(memrpc_minirpc_stress_config_test PRIVATE minirpc_stress_config GTest::gtest_main)
  add_test(NAME memrpc_minirpc_stress_config_test COMMAND memrpc_minirpc_stress_config_test)
  set_tests_properties(memrpc_minirpc_stress_config_test PROPERTIES LABELS "stress")
endif()
```

Update `CMakeLists.txt` (top-level) to add the toggle:

```cmake
option(MEMRPC_ENABLE_STRESS_TESTS "Enable stress test targets" OFF)
option(MEMRPC_ENABLE_FUZZ "Enable fuzz targets" OFF)
```

**Step 2: Run test to verify it fails**

Run:

```bash
cmake -S . -B build_stress -DMEMRPC_ENABLE_STRESS_TESTS=ON
cmake --build build_stress
ctest --test-dir build_stress -L stress --output-on-failure
```

Expected: FAIL (because `ParseStressConfigFromEnv()` not implemented yet).

**Step 3: Write minimal implementation**

Create `tests/apps/minirpc/minirpc_stress_config.h`:

```cpp
#ifndef APPS_MINIRPC_MINIRPC_STRESS_CONFIG_H_
#define APPS_MINIRPC_MINIRPC_STRESS_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

struct StressConfig {
  int durationSec = 60;
  int warmupSec = 5;
  int threads = 1;
  int echoWeight = 70;
  int addWeight = 20;
  int sleepWeight = 10;
  int maxSleepMs = 5;
  int highPriorityPct = 20;
  int burstIntervalMs = 1000;
  int burstDurationMs = 200;
  int burstMultiplier = 3;
  int noProgressTimeoutSec = 30;
  uint32_t maxRequestBytes = memrpc::kDefaultMaxRequestBytes;
  uint32_t maxResponseBytes = memrpc::kDefaultMaxResponseBytes;
  uint64_t seed = 0;
  std::vector<std::size_t> payloadSizes;
};

StressConfig ParseStressConfigFromEnv();

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_MINIRPC_STRESS_CONFIG_H_
```

Create `tests/apps/minirpc/minirpc_stress_config.cpp`:

```cpp
#include "apps/minirpc/minirpc_stress_config.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

int GetEnvInt(const char* name, int default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return default_value;
  }
  try {
    const int parsed = std::stoi(value);
    return parsed >= 0 ? parsed : default_value;
  } catch (const std::exception&) {
    return default_value;
  }
}

uint64_t GetEnvUint64(const char* name, uint64_t default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return default_value;
  }
  try {
    return static_cast<uint64_t>(std::stoull(value));
  } catch (const std::exception&) {
    return default_value;
  }
}

std::vector<std::size_t> ParseCsvSizes(const char* value) {
  std::vector<std::size_t> sizes;
  if (value == nullptr || *value == '\0') {
    return sizes;
  }
  std::stringstream ss(value);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }
    try {
      const std::size_t parsed = static_cast<std::size_t>(std::stoull(token));
      sizes.push_back(parsed);
    } catch (const std::exception&) {
      continue;
    }
  }
  return sizes;
}

std::vector<std::size_t> DefaultPayloadSizes(uint32_t maxRequestBytes) {
  std::vector<std::size_t> sizes = {0, 16, 128, 512, 1024, 2048, 4096, 4090};
  sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                             [maxRequestBytes](std::size_t v) {
                               return v > maxRequestBytes;
                             }),
              sizes.end());
  if (sizes.empty()) {
    sizes.push_back(0);
  }
  return sizes;
}

void ClampPayloadSizes(std::vector<std::size_t>* sizes, uint32_t maxRequestBytes) {
  if (sizes == nullptr) {
    return;
  }
  sizes->erase(std::remove_if(sizes->begin(), sizes->end(),
                              [maxRequestBytes](std::size_t v) {
                                return v > maxRequestBytes;
                              }),
               sizes->end());
  if (sizes->empty()) {
    sizes->push_back(0);
  }
}

}  // namespace

StressConfig ParseStressConfigFromEnv() {
  StressConfig config;
  config.durationSec = GetEnvInt("MEMRPC_STRESS_DURATION_SEC", config.durationSec);
  config.warmupSec = GetEnvInt("MEMRPC_STRESS_WARMUP_SEC", config.warmupSec);
  config.threads = GetEnvInt("MEMRPC_STRESS_THREADS", config.threads);
  config.echoWeight = GetEnvInt("MEMRPC_STRESS_ECHO_WEIGHT", config.echoWeight);
  config.addWeight = GetEnvInt("MEMRPC_STRESS_ADD_WEIGHT", config.addWeight);
  config.sleepWeight = GetEnvInt("MEMRPC_STRESS_SLEEP_WEIGHT", config.sleepWeight);
  config.maxSleepMs = GetEnvInt("MEMRPC_STRESS_MAX_SLEEP_MS", config.maxSleepMs);
  config.highPriorityPct = GetEnvInt("MEMRPC_STRESS_HIGH_PRIORITY_PCT", config.highPriorityPct);
  config.burstIntervalMs = GetEnvInt("MEMRPC_STRESS_BURST_INTERVAL_MS", config.burstIntervalMs);
  config.burstDurationMs = GetEnvInt("MEMRPC_STRESS_BURST_durationMs", config.burstDurationMs);
  config.burstMultiplier = GetEnvInt("MEMRPC_STRESS_BURST_MULTIPLIER", config.burstMultiplier);
  config.noProgressTimeoutSec = GetEnvInt("MEMRPC_STRESS_NO_PROGRESS_SEC", config.noProgressTimeoutSec);
  config.maxRequestBytes = static_cast<uint32_t>(
      GetEnvInt("MEMRPC_STRESS_MAX_REQUEST_BYTES", static_cast<int>(config.maxRequestBytes)));
  config.maxResponseBytes = static_cast<uint32_t>(
      GetEnvInt("MEMRPC_STRESS_MAX_RESPONSE_BYTES", static_cast<int>(config.maxResponseBytes)));
  config.seed = GetEnvUint64("MEMRPC_STRESS_SEED", config.seed);

  const char* sizes = std::getenv("MEMRPC_STRESS_PAYLOAD_SIZES");
  if (sizes != nullptr && *sizes != '\0') {
    config.payloadSizes = ParseCsvSizes(sizes);
  } else {
    config.payloadSizes = DefaultPayloadSizes(config.maxRequestBytes);
  }
  ClampPayloadSizes(&config.payloadSizes, config.maxRequestBytes);

  if (config.threads <= 0) {
    config.threads = 1;
  }
  if (config.burstIntervalMs < 0) {
    config.burstIntervalMs = 0;
  }
  if (config.burstDurationMs < 0) {
    config.burstDurationMs = 0;
  }
  if (config.burstMultiplier < 1) {
    config.burstMultiplier = 1;
  }

  return config;
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build_stress
ctest --test-dir build_stress -L stress --output-on-failure
```

Expected: PASS.

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_stress_config.h \
  tests/apps/minirpc/minirpc_stress_config.cpp \
  tests/apps/minirpc/minirpc_stress_config_test.cpp \
  tests/apps/minirpc/CMakeLists.txt \
  CMakeLists.txt

git commit -m "feat: add minirpc stress config parser"
```

---

### Task 2: Add Stress Runner Binary + Smoke Test

**Files:**
- Create: `tests/apps/minirpc/minirpc_stress_runner.cpp`
- Modify: `tests/apps/minirpc/CMakeLists.txt`

**Step 1: Write the failing test**

Extend `tests/apps/minirpc/CMakeLists.txt` within the `MEMRPC_ENABLE_STRESS_TESTS` block:

```cmake
  add_executable(memrpc_minirpc_stress_runner minirpc_stress_runner.cpp)
  target_link_libraries(memrpc_minirpc_stress_runner PRIVATE minirpc_stress_config minirpc_demo)
  target_compile_features(memrpc_minirpc_stress_runner PRIVATE cxx_std_17)

  add_test(NAME memrpc_minirpc_stress_smoke COMMAND memrpc_minirpc_stress_runner)
  set_tests_properties(memrpc_minirpc_stress_smoke PROPERTIES
    LABELS "stress"
    ENVIRONMENT
      "MEMRPC_STRESS_DURATION_SEC=1;MEMRPC_STRESS_WARMUP_SEC=0;MEMRPC_STRESS_THREADS=1;MEMRPC_STRESS_ECHO_WEIGHT=100;MEMRPC_STRESS_ADD_WEIGHT=0;MEMRPC_STRESS_SLEEP_WEIGHT=0;MEMRPC_STRESS_PAYLOAD_SIZES=0,16;MEMRPC_STRESS_MAX_SLEEP_MS=1;MEMRPC_STRESS_SEED=1"
  )
```

**Step 2: Run test to verify it fails**

```bash
cmake -S . -B build_stress -DMEMRPC_ENABLE_STRESS_TESTS=ON
cmake --build build_stress
ctest --test-dir build_stress -L stress --output-on-failure
```

Expected: FAIL (runner source not implemented yet).

**Step 3: Write minimal implementation**

Create `tests/apps/minirpc/minirpc_stress_runner.cpp`:

```cpp
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
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

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

namespace Mem = ::OHOS::Security::VirusProtectionService::MemRpc;

uint64_t MonotonicNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void CloseHandles(Mem::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
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
    HILOGE("minirpc stress server start failed");
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
    HILOGE("stress bootstrap open session failed");
    return false;
  }
  CloseHandles(unusedHandles);

  const Mem::BootstrapHandles serverHandles = bootstrap->server_handles();
  const pid_t child = fork();
  if (child == 0) {
    RunChild(serverHandles, static_cast<uint32_t>(config.threads));
    return true;
  }
  if (child < 0) {
    HILOGE("stress fork failed");
    return false;
  }

  WritePidFile(child);

  Mem::RpcClient client(bootstrap);
  if (client.Init() != Mem::StatusCode::Ok) {
    HILOGE("stress client init failed");
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
          status = Mem::InvokeTypedSync(&client, Mem::Opcode::MiniEcho, request, &reply, priority);
        } else if (kind == RpcKind::Add) {
          AddRequest request{static_cast<int32_t>(rng()), static_cast<int32_t>(rng())};
          AddReply reply;
          status = Mem::InvokeTypedSync(&client, Mem::Opcode::MiniAdd, request, &reply, priority);
        } else {
          const uint32_t delayMs = static_cast<uint32_t>(rng() % std::max(1, config.maxSleepMs));
          SleepRequest request{delayMs};
          SleepReply reply;
          status = Mem::InvokeTypedSync(&client, Mem::Opcode::MiniSleep, request, &reply, priority);
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
    HILOGE("stress failed: %s", state.error.c_str());
    return false;
  }

  HILOGI("stress ok: ops=%{public}llu", static_cast<unsigned long long>(state.okCount.load()));
  return true;
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

int main() {
  using namespace OHOS::Security::VirusProtectionService::MiniRpc;
  const StressConfig config = ParseStressConfigFromEnv();
  HILOGI("stress start: duration=%{public}d warmup=%{public}d threads=%{public}d",
        config.durationSec, config.warmupSec, config.threads);
  const bool ok = RunStress(config);
  return ok ? 0 : 1;
}
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build_stress
ctest --test-dir build_stress -L stress --output-on-failure
```

Expected: PASS.

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_stress_runner.cpp \
  tests/apps/minirpc/CMakeLists.txt

git commit -m "feat: add minirpc stress runner"
```

---

### Task 3: Add Stress Run Script with RSS Sampling

**Files:**
- Create: `tools/stress/run_minirpc_stress.sh`

**Step 1: Write the failing test**

Create a stub script that exits non-zero:

```bash
#!/usr/bin/env bash
set -euo pipefail

echo "TODO: implement"
exit 1
```

Run:

```bash
bash tools/stress/run_minirpc_stress.sh --help
```

Expected: exit code 1.

**Step 2: Write minimal implementation**

Replace with:

```bash
#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build_stress}"
OUT_DIR="${OUT_DIR:-stress_out}"
SAMPLE_INTERVAL_SEC="${SAMPLE_INTERVAL_SEC:-5}"

RUNNER="${BUILD_DIR}/tests/apps/minirpc/memrpc_minirpc_stress_runner"
if [[ ! -x "${RUNNER}" ]]; then
  echo "stress runner not found: ${RUNNER}"
  exit 1
fi

mkdir -p "${OUT_DIR}"
PID_FILE="${OUT_DIR}/pids.txt"
LOG_FILE="${OUT_DIR}/stress.log"
RSS_CSV="${OUT_DIR}/rss.csv"

: > "${RSS_CSV}"
echo "timestamp,parent_rss_kb,child_rss_kb" >> "${RSS_CSV}"

MEMRPC_STRESS_PID_FILE="${PID_FILE}" "${RUNNER}" > "${LOG_FILE}" 2>&1 &
RUNNER_PID=$!

for _ in $(seq 1 50); do
  if [[ -f "${PID_FILE}" ]]; then
    break
  fi
  sleep 0.1
done

CHILD_PID="$(grep -m1 '^child_pid=' "${PID_FILE}" | cut -d= -f2 || true)"

while kill -0 "${RUNNER_PID}" 2>/dev/null; do
  TS=$(date +%s)
  PARENT_RSS=$(grep -m1 '^VmRSS:' "/proc/${RUNNER_PID}/status" 2>/dev/null | awk '{print $2}' || echo 0)
  CHILD_RSS=0
  if [[ -n "${CHILD_PID}" && -e "/proc/${CHILD_PID}/status" ]]; then
    CHILD_RSS=$(grep -m1 '^VmRSS:' "/proc/${CHILD_PID}/status" 2>/dev/null | awk '{print $2}' || echo 0)
  fi
  echo "${TS},${PARENT_RSS},${CHILD_RSS}" >> "${RSS_CSV}"
  sleep "${SAMPLE_INTERVAL_SEC}"
done

wait "${RUNNER_PID}"
EXIT_CODE=$?

echo "exit_code=${EXIT_CODE}"
exit "${EXIT_CODE}"
```

**Step 3: Run test to verify it passes**

```bash
bash tools/stress/run_minirpc_stress.sh --help || true
```

Expected: exits 0 and prints usage (if you decide to add a `--help` branch), otherwise skip this step and validate by running a 1-second stress run from Task 2’s build.

**Step 4: Commit**

```bash
git add tools/stress/run_minirpc_stress.sh

git commit -m "feat: add stress run script"
```

---

### Task 4: Add Fuzz Target (minirpc codec)

**Files:**
- Modify: `tests/CMakeLists.txt`
- Create: `tests/fuzz/CMakeLists.txt`
- Create: `tests/fuzz/minirpc_codec_fuzz.cpp`

**Step 1: Write the failing test**

Add fuzz subdirectory when enabled in `tests/CMakeLists.txt`:

```cmake
if (MEMRPC_ENABLE_FUZZ)
  add_subdirectory(fuzz)
endif()
```

Add `tests/fuzz/CMakeLists.txt`:

```cmake
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MEMRPC_ENABLE_FUZZ requires Clang")
endif()

add_executable(minirpc_codec_fuzz minirpc_codec_fuzz.cpp)
set_target_properties(minirpc_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

target_link_libraries(minirpc_codec_fuzz PRIVATE minirpc_demo)

target_compile_options(minirpc_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

target_link_options(minirpc_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME memrpc_minirpc_codec_fuzz_smoke COMMAND minirpc_codec_fuzz -runs=1)
set_tests_properties(memrpc_minirpc_codec_fuzz_smoke PROPERTIES LABELS "fuzz")
```

**Step 2: Run test to verify it fails**

```bash
cmake -S . -B build_fuzz -DMEMRPC_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz
ctest --test-dir build_fuzz -L fuzz --output-on-failure
```

Expected: FAIL (fuzzer source not implemented yet).

**Step 3: Write minimal implementation**

Create `tests/fuzz/minirpc_codec_fuzz.cpp`:

```cpp
#include <cstddef>
#include <cstdint>
#include <vector>

#include "apps/minirpc/common/minirpc_codec.h"

using namespace OHOS::Security::VirusProtectionService::MiniRpc;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> bytes(data, data + size);

  EchoRequest echoRequest;
  EchoReply echoReply;
  AddRequest addRequest;
  AddReply addReply;
  SleepRequest sleepRequest;
  SleepReply sleepReply;

  const bool echoOk = DecodeMessage<EchoRequest>(bytes, &echoRequest);
  const bool addOk = DecodeMessage<AddRequest>(bytes, &addRequest);
  const bool sleepOk = DecodeMessage<SleepRequest>(bytes, &sleepRequest);

  DecodeMessage<EchoReply>(bytes, &echoReply);
  DecodeMessage<AddReply>(bytes, &addReply);
  DecodeMessage<SleepReply>(bytes, &sleepReply);

  if (echoOk) {
    std::vector<uint8_t> out;
    EncodeMessage<EchoRequest>(echoRequest, &out);
  }
  if (addOk) {
    std::vector<uint8_t> out;
    EncodeMessage<AddRequest>(addRequest, &out);
  }
  if (sleepOk) {
    std::vector<uint8_t> out;
    EncodeMessage<SleepRequest>(sleepRequest, &out);
  }

  return 0;
}
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build_fuzz
ctest --test-dir build_fuzz -L fuzz --output-on-failure
```

Expected: PASS.

**Step 5: Commit**

```bash
git add tests/CMakeLists.txt \
  tests/fuzz/CMakeLists.txt \
  tests/fuzz/minirpc_codec_fuzz.cpp

git commit -m "feat: add minirpc codec fuzz target"
```

---

### Task 5: Document Stress/Fuzz Workflows

**Files:**
- Create: `docs/stress_fuzz_guide.md`

**Step 1: Write the failing test**

Add a placeholder doc and confirm it’s present (no automated test):

```bash
test -f docs/stress_fuzz_guide.md
```

Expected: FAIL (file missing).

**Step 2: Write minimal implementation**

Create `docs/stress_fuzz_guide.md`:

```markdown
# Stress & Fuzz Guide

## Build (Release + Stress)

```bash
cmake -S . -B build_stress -DMEMRPC_ENABLE_STRESS_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_stress
```

## Run 2h Random Stress

```bash
MEMRPC_STRESS_DURATION_SEC=7200 \
MEMRPC_STRESS_WARMUP_SEC=600 \
MEMRPC_STRESS_THREADS=8 \
MEMRPC_STRESS_ECHO_WEIGHT=70 \
MEMRPC_STRESS_ADD_WEIGHT=20 \
MEMRPC_STRESS_SLEEP_WEIGHT=10 \
MEMRPC_STRESS_PAYLOAD_SIZES=0,16,128,512,1024,2048,4096,4090 \
./build_stress/tests/apps/minirpc/memrpc_minirpc_stress_runner
```

## ASan/UBSan/LSan Build

```bash
cmake -S . -B build_asan -DMEMRPC_ENABLE_STRESS_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_asan
ctest --test-dir build_asan -L stress --output-on-failure
```

## TSan Build

```bash
cmake -S . -B build_tsan -DMEMRPC_ENABLE_STRESS_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_tsan
ctest --test-dir build_tsan -L stress --output-on-failure
```

## Fuzz Build (Clang)

```bash
cmake -S . -B build_fuzz -DMEMRPC_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz
./build_fuzz/tests/fuzz/minirpc_codec_fuzz -max_total_time=300
```

## Environment Variables

- `MEMRPC_STRESS_DURATION_SEC` / `MEMRPC_STRESS_WARMUP_SEC`
- `MEMRPC_STRESS_THREADS`
- `MEMRPC_STRESS_ECHO_WEIGHT` / `MEMRPC_STRESS_ADD_WEIGHT` / `MEMRPC_STRESS_SLEEP_WEIGHT`
- `MEMRPC_STRESS_PAYLOAD_SIZES`
- `MEMRPC_STRESS_HIGH_PRIORITY_PCT`
- `MEMRPC_STRESS_MAX_SLEEP_MS`
- `MEMRPC_STRESS_BURST_INTERVAL_MS` / `MEMRPC_STRESS_BURST_durationMs` / `MEMRPC_STRESS_BURST_MULTIPLIER`
- `MEMRPC_STRESS_NO_PROGRESS_SEC`
- `MEMRPC_STRESS_PID_FILE`
```

**Step 3: Run test to verify it passes**

```bash
test -f docs/stress_fuzz_guide.md
```

Expected: exit 0.

**Step 4: Commit**

```bash
git add docs/stress_fuzz_guide.md

git commit -m "feat: document stress and fuzz workflows"
```

---

## Notes

- Stress/fuzz targets only build when explicitly enabled; default `cmake -S . -B build` remains unchanged.
- Use `ctest -L stress` or `ctest -L fuzz` in the dedicated build directories.
