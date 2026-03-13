# Vpsdemo Temp Perf Harness Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build and run a temporary `/tmp` C++ harness that reports idle CPU baseline and stress p50/p99 latency + CPU for vpsdemo.

**Architecture:** A standalone program spawns the registry + engine, initializes a single `VpsClient` session, runs an idle sampling phase followed by a stress phase, and prints CPU/latency statistics. All artifacts are temporary in `/tmp`.

**Tech Stack:** C++17, POSIX (`fork`, `exec`, `/proc`), existing `vpsdemo` + `memrpc` static libs.

---

### Task 1: Create an empty harness file and confirm build fails

**Files:**
- Create: `/tmp/vpsdemo_perf_harness.cpp`

**Step 1: Write the empty file**

```cpp
// /tmp/vpsdemo_perf_harness.cpp
```

**Step 2: Run build to verify it fails**

Run:
```bash
/usr/bin/c++ -std=c++17 -O2 -pthread \
  -I/root/code/demo/mem/demo/vpsdemo/include \
  -I/root/code/demo/mem/include \
  -I/root/code/demo/mem/src \
  -I/root/code/demo/mem/third_party/ohos_sa_mock/include \
  /tmp/vpsdemo_perf_harness.cpp \
  /root/code/demo/mem/demo/vpsdemo/build/libvpsdemo_lib.a \
  /root/code/demo/mem/demo/vpsdemo/build/libmemrpc_core.a \
  /root/code/demo/mem/demo/vpsdemo/build/libohos_sa_mock.a \
  -lrt -o /tmp/vpsdemo_perf_harness
```
Expected: FAIL (no `main`).

**Step 3: Commit**

No commit (temporary `/tmp` file only).

---

### Task 2: Implement harness (full code)

**Files:**
- Modify: `/tmp/vpsdemo_perf_harness.cpp`

**Step 1: Write full implementation**

```cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "iservice_registry.h"
#include "registry_backend.h"
#include "registry_server.h"
#include "ves_bootstrap_interface.h"
#include "ves_client.h"
#include "vpsdemo_types.h"
#include "virus_protection_service_log.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/vpsdemo_perf_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/vpsdemo_perf_service.sock";

std::atomic<bool> g_stop{false};
std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

void SignalHandler(int) {
    g_stop.store(true);
}

struct PerfConfig {
    int idleSeconds = 10;
    int threads = 4;
    int iterations = 2000;
    uint32_t seed = 1;
};

struct PerfStats {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> rpcError{0};
};

pid_t SpawnEngine(const std::string& enginePath) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(enginePath.c_str(), enginePath.c_str(),
              REGISTRY_SOCKET.c_str(), SERVICE_SOCKET.c_str(),
              nullptr);
        _exit(1);
    }
    return pid;
}

void KillAndWait(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

bool ReadTotalJiffies(uint64_t* out) {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return false;
    std::string line;
    if (!std::getline(file, line)) return false;
    std::istringstream iss(line);
    std::string cpu;
    iss >> cpu;
    if (cpu != "cpu") return false;
    uint64_t v = 0;
    uint64_t total = 0;
    while (iss >> v) {
        total += v;
    }
    *out = total;
    return true;
}

bool ReadProcJiffies(pid_t pid, uint64_t* out) {
    std::ifstream file(std::string("/proc/") + std::to_string(pid) + "/stat");
    if (!file.is_open()) return false;
    std::string line;
    if (!std::getline(file, line)) return false;
    const auto rparen = line.rfind(')');
    if (rparen == std::string::npos) return false;
    std::string rest = line.substr(rparen + 2);
    std::istringstream iss(rest);
    std::string state;
    if (!(iss >> state)) return false;
    long long skip = 0;
    for (int i = 0; i < 10; i++) {
        if (!(iss >> skip)) return false;
    }
    uint64_t utime = 0;
    uint64_t stime = 0;
    if (!(iss >> utime >> stime)) return false;
    *out = utime + stime;
    return true;
}

struct CpuSnapshot {
    uint64_t totalJiffies = 0;
    uint64_t procJiffies = 0;
};

bool TakeSnapshot(pid_t pid, CpuSnapshot* snap) {
    if (!snap) return false;
    if (!ReadTotalJiffies(&snap->totalJiffies)) return false;
    if (!ReadProcJiffies(pid, &snap->procJiffies)) return false;
    return true;
}

double CpuPercent(const CpuSnapshot& a, const CpuSnapshot& b, int cpuCount) {
    const uint64_t totalDelta = b.totalJiffies - a.totalJiffies;
    const uint64_t procDelta = b.procJiffies - a.procJiffies;
    if (totalDelta == 0) return 0.0;
    return (static_cast<double>(procDelta) / static_cast<double>(totalDelta)) * 100.0 * cpuCount;
}

PerfConfig ParseArgs(int argc, char* argv[]) {
    PerfConfig config;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--idle-seconds") == 0 && i + 1 < argc) {
            config.idleSeconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.threads = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            config.iterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            config.seed = static_cast<uint32_t>(std::atoi(argv[++i]));
        }
    }
    return config;
}

void WorkerThread(const PerfConfig& config, uint32_t threadId,
                  vpsdemo::VpsClient* client, PerfStats* stats,
                  std::vector<double>* latenciesUs) {
    std::mt19937 rng(config.seed + threadId);
    std::uniform_int_distribution<int> dist(0, 1);
    latenciesUs->reserve(static_cast<size_t>(config.iterations));

    for (int i = 0; i < config.iterations && !g_stop.load(); i++) {
        const bool virus = (dist(rng) == 1);
        const std::string token = virus ? "virus" : "clean";
        const std::string path = "/data/perf_" + token + "_" +
                                 std::to_string(threadId) + "_" + std::to_string(i) + ".apk";

        vpsdemo::ScanFileReply reply;
        const auto t0 = std::chrono::steady_clock::now();
        const auto status = client->ScanFile(path, &reply);
        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        latenciesUs->push_back(us);
        stats->total++;
        if (status != memrpc::StatusCode::Ok) {
            stats->rpcError++;
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    const PerfConfig config = ParseArgs(argc, argv);
    const int cpuCount = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

    // Determine engine path relative to our binary.
    std::string dir = ".";
    std::string argv0(argv[0]);
    auto pos = argv0.rfind('/');
    if (pos != std::string::npos) {
        dir = argv0.substr(0, pos);
    }
    const std::string enginePath = dir + "/vpsdemo_engine_sa";

    // Cleanup stale sockets.
    unlink(REGISTRY_SOCKET.c_str());
    unlink(SERVICE_SOCKET.c_str());

    vpsdemo::RegistryServer registry(REGISTRY_SOCKET);
    registry.SetLoadCallback([&](int32_t sa_id) -> bool {
        if (sa_id != vpsdemo::VPS_BOOTSTRAP_SA_ID) return false;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) return true;
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return true;
    });
    registry.SetUnloadCallback([&](int32_t sa_id) {
        if (sa_id != vpsdemo::VPS_BOOTSTRAP_SA_ID) return;
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    });

    if (!registry.Start()) {
        std::cerr << "registry start failed" << std::endl;
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    if (g_engine_pid < 0) {
        std::cerr << "engine spawn failed" << std::endl;
        registry.Stop();
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto backend = std::make_shared<vpsdemo::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    vpsdemo::VpsClient::RegisterProxyFactory();

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID, 5000);
    if (remote == nullptr) {
        std::cerr << "LoadSystemAbility failed" << std::endl;
        KillAndWait(g_engine_pid);
        registry.Stop();
        return 1;
    }

    auto client = std::make_unique<vpsdemo::VpsClient>(remote);
    if (client->Init() != memrpc::StatusCode::Ok) {
        std::cerr << "VpsClient init failed" << std::endl;
        KillAndWait(g_engine_pid);
        registry.Stop();
        return 1;
    }

    // --- Idle phase ---
    CpuSnapshot idleStartClient{};
    CpuSnapshot idleStartEngine{};
    CpuSnapshot idleEndClient{};
    CpuSnapshot idleEndEngine{};

    TakeSnapshot(getpid(), &idleStartClient);
    TakeSnapshot(g_engine_pid, &idleStartEngine);
    std::this_thread::sleep_for(std::chrono::seconds(config.idleSeconds));
    TakeSnapshot(getpid(), &idleEndClient);
    TakeSnapshot(g_engine_pid, &idleEndEngine);

    const double idleClientCpu = CpuPercent(idleStartClient, idleEndClient, cpuCount);
    const double idleEngineCpu = CpuPercent(idleStartEngine, idleEndEngine, cpuCount);

    // --- Stress phase ---
    PerfStats stats;
    std::vector<std::vector<double>> perThreadLat(config.threads);
    std::vector<std::thread> workers;
    workers.reserve(config.threads);

    CpuSnapshot stressStartClient{};
    CpuSnapshot stressStartEngine{};
    CpuSnapshot stressEndClient{};
    CpuSnapshot stressEndEngine{};

    const auto stressStart = std::chrono::steady_clock::now();
    TakeSnapshot(getpid(), &stressStartClient);
    TakeSnapshot(g_engine_pid, &stressStartEngine);

    for (int t = 0; t < config.threads; t++) {
        workers.emplace_back(WorkerThread, std::cref(config),
                             static_cast<uint32_t>(t), client.get(), &stats,
                             &perThreadLat[static_cast<size_t>(t)]);
    }
    for (auto& w : workers) {
        w.join();
    }

    TakeSnapshot(getpid(), &stressEndClient);
    TakeSnapshot(g_engine_pid, &stressEndEngine);
    const auto stressEnd = std::chrono::steady_clock::now();

    const double stressClientCpu = CpuPercent(stressStartClient, stressEndClient, cpuCount);
    const double stressEngineCpu = CpuPercent(stressStartEngine, stressEndEngine, cpuCount);

    std::vector<double> allLat;
    size_t totalCount = 0;
    for (const auto& v : perThreadLat) {
        totalCount += v.size();
    }
    allLat.reserve(totalCount);
    for (auto& v : perThreadLat) {
        allLat.insert(allLat.end(), v.begin(), v.end());
    }
    std::sort(allLat.begin(), allLat.end());

    double p50 = 0.0;
    double p99 = 0.0;
    if (!allLat.empty()) {
        p50 = allLat[allLat.size() * 50 / 100];
        p99 = allLat[allLat.size() * 99 / 100];
    }

    const double durationSec = std::chrono::duration_cast<std::chrono::duration<double>>(
        stressEnd - stressStart).count();
    const double ops = static_cast<double>(stats.total.load());
    const double opsPerSec = (durationSec > 0.0) ? (ops / durationSec) : 0.0;

    std::cout << "=== VPSDEMO PERF RESULTS ===" << std::endl;
    std::cout << "idle_seconds=" << config.idleSeconds
              << " idle_client_cpu%=" << idleClientCpu
              << " idle_engine_cpu%=" << idleEngineCpu
              << " idle_total_cpu%=" << (idleClientCpu + idleEngineCpu) << std::endl;
    std::cout << "stress_threads=" << config.threads
              << " iterations=" << config.iterations
              << " duration_s=" << durationSec
              << " ops=" << ops
              << " ops_s=" << opsPerSec << std::endl;
    std::cout << "p50_us=" << p50
              << " p99_us=" << p99
              << " rpc_error=" << stats.rpcError.load() << std::endl;
    std::cout << "stress_client_cpu%=" << stressClientCpu
              << " stress_engine_cpu%=" << stressEngineCpu
              << " stress_total_cpu%=" << (stressClientCpu + stressEngineCpu) << std::endl;

    client->Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
    registry.Stop();

    return 0;
}
```

**Step 2: Build to verify it compiles**

Run:
```bash
/usr/bin/c++ -std=c++17 -O2 -pthread \
  -I/root/code/demo/mem/demo/vpsdemo/include \
  -I/root/code/demo/mem/include \
  -I/root/code/demo/mem/src \
  -I/root/code/demo/mem/third_party/ohos_sa_mock/include \
  /tmp/vpsdemo_perf_harness.cpp \
  /root/code/demo/mem/demo/vpsdemo/build/libvpsdemo_lib.a \
  /root/code/demo/mem/demo/vpsdemo/build/libmemrpc_core.a \
  /root/code/demo/mem/demo/vpsdemo/build/libohos_sa_mock.a \
  -lrt -o /tmp/vpsdemo_perf_harness
```
Expected: PASS, `/tmp/vpsdemo_perf_harness` created.

**Step 3: Commit**

No commit (temporary `/tmp` file only).

---

### Task 3: Run idle + stress measurements

**Files:**
- None

**Step 1: Run the harness**

Run:
```bash
/tmp/vpsdemo_perf_harness --idle-seconds 10 --threads 4 --iterations 2000
```

Expected: Output with idle CPU%, p50/p99, ops/s, and stress CPU%.

**Step 2: If needed, re-run with different load**

Run:
```bash
/tmp/vpsdemo_perf_harness --idle-seconds 10 --threads 8 --iterations 4000
```

Expected: Second sample for comparison.

**Step 3: Commit**

No commit (temporary runtime only).

