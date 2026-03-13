# Vpsdemo Stress Samples Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add shared sample rules for scan behavior and a standalone stress client that runs randomized concurrent scans and validates results.

**Architecture:** Introduce a shared `EvaluateSamplePath()` helper used by both the server scan handler and the stress client. The stress client self-hosts a registry server, spawns the engine SA, then runs concurrent scan requests and validates deterministic results.

**Tech Stack:** C++17, CMake, memrpc, OHOS SA mock registry, `virus_protection_service_log.h`.

---

### Task 1: Implement Sample Rules + Stress Client (Single End-to-End Slice)

**Files:**
- Create: `demo/vpsdemo/include/vesdemo_sample_rules.h`
- Create: `demo/vpsdemo/src/vesdemo_sample_rules.cpp`
- Create: `demo/vpsdemo/src/vesdemo_stress_client.cpp`
- Modify: `demo/vpsdemo/src/vpsdemo_service.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test (stress client with explicit expectations)**

Create `demo/vpsdemo/src/vesdemo_stress_client.cpp` with a local expected mapping table
that marks `eicar` and `sleep*` samples as `threat=1`. This will fail against the current
server logic (which only matches `"virus"`).

Key scaffold (trimmed for focus; full file in Step 3):

```cpp
struct ExpectedSample {
    std::string token;
    int threatLevel;
    bool allowCrash;
};

const std::vector<ExpectedSample> kSamples = {
    {"clean", 0, false},
    {"virus", 1, false},
    {"eicar", 1, false},
    {"sleep3", 1, false},
    {"sleep100", 1, false},
    {"crash", 0, true},
};
```

**Step 2: Run test to verify it fails**

Run:

```bash
cmake -S demo/vpsdemo -B demo/vpsdemo/build -DVPSDEMO_ENABLE_TESTS=OFF
cmake --build demo/vpsdemo/build
./demo/vpsdemo/build/vpsdemo_stress_client --threads 2 --iterations 20 --seed 1
```

Expected: non-zero exit with `mismatch > 0` (because server lacks `eicar`/`sleep*` rules).

**Step 3: Write minimal implementation (shared helper + service integration)**

Create `demo/vpsdemo/include/vesdemo_sample_rules.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace vpsdemo {

struct SampleBehavior {
    int threatLevel = 0;
    uint32_t sleepMs = 0;
    bool shouldCrash = false;
};

SampleBehavior EvaluateSamplePath(const std::string& path);

}  // namespace vpsdemo
```

Create `demo/vpsdemo/src/vesdemo_sample_rules.cpp`:

```cpp
#include "vesdemo_sample_rules.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace vpsdemo {
namespace {

constexpr uint32_t MAX_SLEEP_MS = 5000;

uint32_t ParseSleepMs(const std::string& path) {
    const std::string key = "sleep";
    auto pos = path.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    if (pos >= path.size() || !std::isdigit(path[pos])) return 0;
    uint32_t seconds = 0;
    while (pos < path.size() && std::isdigit(path[pos])) {
        seconds = seconds * 10 + static_cast<uint32_t>(path[pos] - '0');
        pos++;
    }
    uint32_t ms = seconds * 1000;
    return std::min(ms, MAX_SLEEP_MS);
}

}  // namespace

SampleBehavior EvaluateSamplePath(const std::string& path) {
    SampleBehavior behavior;
    if (path.find("crash") != std::string::npos) {
        behavior.shouldCrash = true;
    }
    behavior.sleepMs = ParseSleepMs(path);
    if (path.find("virus") != std::string::npos ||
        path.find("eicar") != std::string::npos) {
        behavior.threatLevel = 1;
    }
    return behavior;
}

}  // namespace vpsdemo
```

Update `demo/vpsdemo/src/vpsdemo_service.cpp` scan handler:

```cpp
const auto behavior = EvaluateSamplePath(request.file_path);
if (behavior.shouldCrash) {
    HILOGE("ScanFile(%{public}s): crash requested", request.file_path.c_str());
    std::abort();
}
if (behavior.sleepMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(behavior.sleepMs));
}
result.threat_level = behavior.threatLevel;
```

**Step 4: Update stress client to use shared helper**

Replace the local table with `EvaluateSamplePath()` for expected behavior, while
still using a fixed list of sample tokens to generate paths.

Key parts to include in `vesdemo_stress_client.cpp`:

```cpp
struct SampleToken {
    std::string token;
    bool allowCrash;
};

const std::vector<SampleToken> kTokens = {
    {"clean", false},
    {"virus", false},
    {"eicar", false},
    {"sleep3", false},
    {"sleep100", false},
    {"crash", true},
};
```

For each generated path:

```cpp
const auto behavior = EvaluateSamplePath(path);
if (behavior.shouldCrash && !includeCrash) continue;
ScanFileReply reply;
auto status = client->ScanFile(path, &reply);
if (status != memrpc::StatusCode::Ok) { rpcError++; continue; }
if (reply.threat_level != behavior.threatLevel) { mismatch++; }
```

Add self-hosted registry + engine spawn (adapt from `demo/vpsdemo/src/vesdemo_supervisor.cpp`):

```cpp
vpsdemo::RegistryServer registry(registrySocket);
registry.SetLoadCallback([&](int32_t sa_id) -> bool { /* spawn engine */ });
registry.SetUnloadCallback([&](int32_t sa_id) { /* kill engine */ });
registry.Start();
```

**Step 5: Update CMake**

Add the new sources to `vpsdemo_lib`, and add the new executable:

```cmake
add_library(vpsdemo_lib
  ...
  src/vesdemo_sample_rules.cpp
)

add_executable(vpsdemo_stress_client src/vesdemo_stress_client.cpp)
target_link_libraries(vpsdemo_stress_client PRIVATE vpsdemo_lib)
```

**Step 6: Run test to verify it passes**

```bash
cmake -S demo/vpsdemo -B demo/vpsdemo/build -DVPSDEMO_ENABLE_TESTS=OFF
cmake --build demo/vpsdemo/build
./demo/vpsdemo/build/vpsdemo_stress_client --threads 2 --iterations 50 --seed 1
```

Expected: exit code `0`, `mismatch=0`, `rpc_error=0` (with `--include-crash` off).

**Step 7: Commit**

```bash
git add demo/vpsdemo/CMakeLists.txt \
  demo/vpsdemo/include/vesdemo_sample_rules.h \
  demo/vpsdemo/src/vesdemo_sample_rules.cpp \
  demo/vpsdemo/src/vpsdemo_service.cpp \
  demo/vpsdemo/src/vesdemo_stress_client.cpp
git commit -m "feat: add vpsdemo stress samples and shared rules"
```

---
