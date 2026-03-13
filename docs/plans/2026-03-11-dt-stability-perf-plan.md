# TODO: DT Stability & Perf Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add short-duration DT stability and performance regression tests for memrpc and minirpc, with per-machine baselines under `build/`.

**Architecture:** Add four new GTest binaries (memrpc/minirpc stability + perf). Each perf test records ops/s and p99 latency, compares against a per-machine baseline file, and auto-updates only on improvement. Stability tests run short randomized workloads with progress watchdogs. To honor "independent build first", DT tests are gated by a CMake option (`MEMRPC_ENABLE_DT_TESTS`) that is OFF during phase 1, verified in a dedicated build dir (`build-dt`), then flipped ON in a final phase to join mainline build.

**Tech Stack:** C++17, GTest, CMake/CTest.

---

### Task 0: Add DT test build gate (phase 1)

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/memrpc/CMakeLists.txt`
- Modify: `tests/apps/minirpc/CMakeLists.txt`

**Step 1: Add a DT test CMake option (default OFF for phase 1)**

Add a new option next to the existing stress/fuzz options:

```cmake
option(MEMRPC_ENABLE_DT_TESTS "Enable DT stability/perf tests" OFF)
```

**Step 2: Add DT test guard blocks (empty placeholders for now)**

Add empty `if (MEMRPC_ENABLE_DT_TESTS)` blocks in the two test CMakeLists so later tasks can insert DT targets under the guard:

```cmake
if (MEMRPC_ENABLE_DT_TESTS)
  # DT stability/perf tests (added in later tasks).
endif()
```

**Step 3: Configure dedicated DT build directory**

Run:

```bash
cmake -S . -B build-dt -DMEMRPC_ENABLE_DT_TESTS=ON
```

Expected: configure succeeds.

**Step 4: Commit**

```bash
git add CMakeLists.txt tests/memrpc/CMakeLists.txt tests/apps/minirpc/CMakeLists.txt
git commit -m "feat: add dt test build option"
```

### Task 1: Add memrpc DT stability test

**Files:**
- Create: `tests/memrpc/dt_stability_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`
- Test: `ctest --test-dir build-dt -R memrpc_dt_stability_test -V`

**Step 1: Write the failing test**

Create `tests/memrpc/dt_stability_test.cpp` with a deliberate failure at the end of the test so we can confirm it runs:

```cpp
#include <gtest/gtest.h>

TEST(DtStabilityTest, Runs) {
  FAIL() << "intentional failure to verify test runs";
}
```

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_dt_stability_test -V
```

Expected: test fails with "intentional failure".

**Step 3: Write minimal implementation**

Replace the file content with the real short-run stability test:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

void CloseHandles(memrpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
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
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw == 0 ? 1u : hw)));
  return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", default_threads));
}

std::vector<uint8_t> MakePayload(size_t size, uint8_t seed) {
  return std::vector<uint8_t>(size, static_cast<uint8_t>(seed));
}

}  // namespace

TEST(DtStabilityTest, ShortRandomLoadStaysHealthy) {
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int progress_timeout_ms = GetEnvInt("MEMRPC_DT_PROGRESS_TIMEOUT_MS", 200);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  memrpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), memrpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  memrpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  std::atomic<uint64_t> success{0};
  std::atomic<memrpc::StatusCode> first_error{memrpc::StatusCode::Ok};
  std::atomic<int64_t> last_success_ms{0};

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(duration_ms);

  auto now_ms = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  last_success_ms.store(now_ms());

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (uint32_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([&, i]() {
      std::mt19937 rng(static_cast<uint32_t>(i + 1));
      const std::vector<size_t> sizes = {0, 128, 512, 2048, 4096};
      while (std::chrono::steady_clock::now() < deadline) {
        const size_t payload_size = sizes[rng() % sizes.size()];
        memrpc::RpcCall call;
        call.opcode = memrpc::Opcode::ScanFile;
        call.payload = MakePayload(payload_size, static_cast<uint8_t>(i));

        auto future = client.InvokeAsync(call);
        memrpc::RpcReply reply;
        memrpc::StatusCode status = future.Wait(&reply);
        if (status != memrpc::StatusCode::Ok) {
          first_error.store(status);
          return;
        }
        ++success;
        last_success_ms.store(now_ms());
      }
    });
  }

  std::atomic<bool> progress_ok{true};
  std::thread watchdog([&]() {
    while (std::chrono::steady_clock::now() < deadline) {
      const int64_t last = last_success_ms.load();
      if (now_ms() - last > progress_timeout_ms) {
        progress_ok.store(false);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  for (auto& t : workers) {
    t.join();
  }
  watchdog.join();

  client.Shutdown();
  server.Stop();

  EXPECT_EQ(first_error.load(), memrpc::StatusCode::Ok);
  EXPECT_TRUE(progress_ok.load());
  EXPECT_GT(success.load(), 0u);
}
```

Update `tests/memrpc/CMakeLists.txt` to register the DT test under the guard and add the label:

```cmake
if (MEMRPC_ENABLE_DT_TESTS)
  memrpc_add_test(memrpc_dt_stability_test dt_stability_test.cpp)
  set_tests_properties(memrpc_dt_stability_test PROPERTIES LABELS "dt_stability")
endif()
```

**Step 4: Run test to verify it passes**

Run:

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_dt_stability_test -V
```

Expected: PASS.

**Step 5: Commit**

```bash
git add tests/memrpc/dt_stability_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: add memrpc dt stability test"
```

---

### Task 2: Add memrpc DT perf test

**Files:**
- Create: `tests/memrpc/dt_perf_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`
- Test: `ctest --test-dir build-dt -R memrpc_dt_perf_test -V`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

TEST(DtPerfTest, Runs) {
  FAIL() << "intentional failure to verify test runs";
}
```

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_dt_perf_test -V
```

Expected: FAIL with "intentional failure".

**Step 3: Write minimal implementation**

Replace file with a short perf test that records ops/s + p99 and updates a baseline:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

void CloseHandles(memrpc::BootstrapHandles& h) {
  if (h.shmFd >= 0) close(h.shmFd);
  if (h.highReqEventFd >= 0) close(h.highReqEventFd);
  if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
  if (h.respEventFd >= 0) close(h.respEventFd);
  if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
  if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
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
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw == 0 ? 1u : hw)));
  return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", default_threads));
}

std::filesystem::path BaselinePath() {
  const char* dir = std::getenv("MEMRPC_DT_BASELINE_DIR");
  std::filesystem::path base = (dir && *dir) ? dir : (std::filesystem::current_path() / "perf_baselines");
  return base / "memrpc_dt_perf.baseline";
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
  output << "# memrpc dt perf baseline\n";
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

TEST(DtPerfTest, ShortPerfBaseline) {
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int warmup_ms = GetEnvInt("MEMRPC_DT_WARMUP_MS", 200);
  const int min_ops = GetEnvInt("MEMRPC_DT_MIN_OPS", 50);
  const int max_p99_us = GetEnvInt("MEMRPC_DT_MAX_P99_US", 20000);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  memrpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), memrpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  memrpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  const std::vector<std::pair<const char*, size_t>> cases = {
      {"echo_0B", 0},
      {"echo_4KB", 4096},
  };

  std::map<std::string, double> baseline = LoadBaseline(BaselinePath());
  bool baseline_updated = false;

  for (const auto& c : cases) {
    const std::string case_name = c.first;
    const size_t payload_size = c.second;

    // Warmup (single thread).
    const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
    while (std::chrono::steady_clock::now() < warmup_end) {
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.payload.assign(payload_size, 'x');
      auto future = client.InvokeAsync(call);
      memrpc::RpcReply reply;
      (void)future.Wait(&reply);
    }

    std::atomic<uint64_t> ops{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::milliseconds(duration_ms);

    for (uint32_t i = 0; i < thread_count; ++i) {
      workers.emplace_back([&, i]() {
        while (std::chrono::steady_clock::now() < end) {
          memrpc::RpcCall call;
          call.opcode = memrpc::Opcode::ScanFile;
          call.payload.assign(payload_size, static_cast<uint8_t>('a' + (i % 8)));
          auto future = client.InvokeAsync(call);
          memrpc::RpcReply reply;
          if (future.Wait(&reply) == memrpc::StatusCode::Ok) {
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
      memrpc::RpcCall call;
      call.opcode = memrpc::Opcode::ScanFile;
      call.payload.assign(payload_size, 'x');
      const auto t0 = std::chrono::steady_clock::now();
      auto future = client.InvokeAsync(call);
      memrpc::RpcReply reply;
      memrpc::StatusCode status = future.Wait(&reply);
      const auto t1 = std::chrono::steady_clock::now();
      ASSERT_EQ(status, memrpc::StatusCode::Ok);
      const double us = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
      latencies.push_back(us);
    }

    const double duration_sec = std::max(1, duration_ms) / 1000.0;
    const PerfStats stats = ComputeStats(latencies, duration_sec, ops.load());

    const std::string ops_key = "memrpc." + case_name + ".threads=" + std::to_string(thread_count) + ".ops_per_sec";
    const std::string p99_key = "memrpc." + case_name + ".threads=" + std::to_string(thread_count) + ".p99_us";

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
```

Update `tests/memrpc/CMakeLists.txt` to register the DT perf test under the guard and add the label:

```cmake
if (MEMRPC_ENABLE_DT_TESTS)
  memrpc_add_test(memrpc_dt_perf_test dt_perf_test.cpp)
  set_tests_properties(memrpc_dt_perf_test PROPERTIES LABELS "dt_perf")
endif()
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_dt_perf_test -V
```

Expected: PASS, baseline file created/updated under `build/perf_baselines/`.

**Step 5: Commit**

```bash
git add tests/memrpc/dt_perf_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: add memrpc dt perf test"
```

---

### Task 3: Add minirpc DT stability test

**Files:**
- Create: `tests/apps/minirpc/minirpc_dt_stability_test.cpp`
- Modify: `tests/apps/minirpc/CMakeLists.txt`
- Test: `ctest --test-dir build-dt -R memrpc_minirpc_dt_stability_test -V`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

TEST(MiniRpcDtStabilityTest, Runs) {
  FAIL() << "intentional failure to verify test runs";
}
```

**Step 2: Run test to verify it fails**

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_minirpc_dt_stability_test -V
```

Expected: FAIL with "intentional failure".

**Step 3: Write minimal implementation**

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>

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
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw == 0 ? 1u : hw)));
  return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", default_threads));
}

}  // namespace

TEST(MiniRpcDtStabilityTest, ShortRandomLoadStaysHealthy) {
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int progress_timeout_ms = GetEnvInt("MEMRPC_DT_PROGRESS_TIMEOUT_MS", 200);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MemRpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  std::atomic<uint64_t> success{0};
  std::atomic<MemRpc::StatusCode> first_error{MemRpc::StatusCode::Ok};
  std::atomic<int64_t> last_success_ms{0};

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(duration_ms);

  auto now_ms = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  last_success_ms.store(now_ms());

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (uint32_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([&, i]() {
      std::mt19937 rng(static_cast<uint32_t>(i + 7));
      while (std::chrono::steady_clock::now() < deadline) {
        const int choice = static_cast<int>(rng() % 100);
        MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
        if (choice < 70) {
          EchoReply reply;
          status = client.Echo("ping", &reply);
        } else if (choice < 95) {
          AddReply reply;
          status = client.Add(1, 2, &reply);
        } else {
          SleepReply reply;
          status = client.Sleep(1, &reply);
        }
        if (status != MemRpc::StatusCode::Ok) {
          first_error.store(status);
          return;
        }
        ++success;
        last_success_ms.store(now_ms());
      }
    });
  }

  std::atomic<bool> progress_ok{true};
  std::thread watchdog([&]() {
    while (std::chrono::steady_clock::now() < deadline) {
      const int64_t last = last_success_ms.load();
      if (now_ms() - last > progress_timeout_ms) {
        progress_ok.store(false);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  for (auto& t : workers) {
    t.join();
  }
  watchdog.join();

  client.Shutdown();
  server.Stop();

  EXPECT_EQ(first_error.load(), MemRpc::StatusCode::Ok);
  EXPECT_TRUE(progress_ok.load());
  EXPECT_GT(success.load(), 0u);
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
```

Update `tests/apps/minirpc/CMakeLists.txt` to register the DT stability test under the guard and add the label:

```cmake
if (MEMRPC_ENABLE_DT_TESTS)
  minirpc_add_test(memrpc_minirpc_dt_stability_test minirpc_dt_stability_test.cpp)
  set_tests_properties(memrpc_minirpc_dt_stability_test PROPERTIES LABELS "dt_stability")
endif()
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_minirpc_dt_stability_test -V
```

Expected: PASS.

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_dt_stability_test.cpp tests/apps/minirpc/CMakeLists.txt
git commit -m "feat: add minirpc dt stability test"
```

---

### Task 4: Add minirpc DT perf test

**Files:**
- Create: `tests/apps/minirpc/minirpc_dt_perf_test.cpp`
- Modify: `tests/apps/minirpc/CMakeLists.txt`
- Test: `ctest --test-dir build-dt -R memrpc_minirpc_dt_perf_test -V`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

TEST(MiniRpcDtPerfTest, Runs) {
  FAIL() << "intentional failure to verify test runs";
}
```

**Step 2: Run test to verify it fails**

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_minirpc_dt_perf_test -V
```

Expected: FAIL with "intentional failure".

**Step 3: Write minimal implementation**

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>

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
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw == 0 ? 1u : hw)));
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
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int warmup_ms = GetEnvInt("MEMRPC_DT_WARMUP_MS", 200);
  const int min_ops = GetEnvInt("MEMRPC_DT_MIN_OPS", 50);
  const int max_p99_us = GetEnvInt("MEMRPC_DT_MAX_P99_US", 20000);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MemRpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  const std::vector<std::pair<const char*, size_t>> cases = {
      {"echo_0B", 0},
      {"echo_4KB", 4096},
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
    workers.reserve(thread_count);

    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::milliseconds(duration_ms);

    for (uint32_t i = 0; i < thread_count; ++i) {
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

    const double duration_sec = std::max(1, duration_ms) / 1000.0;
    const PerfStats stats = ComputeStats(latencies, duration_sec, ops.load());

    const std::string ops_key = "minirpc." + case_name + ".threads=" + std::to_string(thread_count) + ".ops_per_sec";
    const std::string p99_key = "minirpc." + case_name + ".threads=" + std::to_string(thread_count) + ".p99_us";

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
```

Update `tests/apps/minirpc/CMakeLists.txt` to register the DT perf test under the guard and add the label:

```cmake
if (MEMRPC_ENABLE_DT_TESTS)
  minirpc_add_test(memrpc_minirpc_dt_perf_test minirpc_dt_perf_test.cpp)
  set_tests_properties(memrpc_minirpc_dt_perf_test PROPERTIES LABELS "dt_perf")
endif()
```

**Step 4: Run test to verify it passes**

```bash
cmake --build build-dt
ctest --test-dir build-dt -R memrpc_minirpc_dt_perf_test -V
```

Expected: PASS, baseline file created/updated under `build/perf_baselines/`.

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_dt_perf_test.cpp tests/apps/minirpc/CMakeLists.txt
git commit -m "feat: add minirpc dt perf test"
```

---

### Task 5: Enable DT tests in mainline build (phase 2)

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Flip DT option default to ON**

Change:

```cmake
option(MEMRPC_ENABLE_DT_TESTS "Enable DT stability/perf tests" OFF)
```

To:

```cmake
option(MEMRPC_ENABLE_DT_TESTS "Enable DT stability/perf tests" ON)
```

**Step 2: Verify dt tests appear in the main build**

Run:

```bash
cmake -S . -B build
ctest --test-dir build -N | rg -n "dt_(stability|perf)"
```

Expected: dt tests are listed.

**Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat: enable dt tests in default build"
```
