# VpsDemo Tests + Fuzz Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add high-value vpsdemo unit tests plus a codec fuzz target and a bounded crash-recovery integration test.

**Architecture:** New tests live under `demo/vpsdemo/tests/` and reuse existing vpsdemo services and memrpc bootstrap to stay realistic without exercising ohos_sa_mock directly. A vpsdemo codec fuzz target lives under `demo/vpsdemo/tests/fuzz/` and is gated by a new `VPSDEMO_ENABLE_FUZZ` option in the vpsdemo CMake build. A single integration-style test spawns the engine SA and validates restart after a crash.

**Tech Stack:** C++17, GoogleTest, libFuzzer (clang), existing memrpc/vpsdemo libraries.

---

### Task 1: Add codec unit tests (ScanFile request/reply)

**Files:**
- Create: `demo/vpsdemo/tests/vpsdemo_codec_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Create `demo/vpsdemo/tests/vpsdemo_codec_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "vpsdemo_codec.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

TEST(VpsDemoCodecTest, ScanFileRequestRoundTrip) {
    ScanFileRequest req;
    req.file_path = "/data/scan/clean.apk";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileRequest>::Encode(req, &bytes));

    ScanFileRequest decoded;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileRequest>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.file_path, req.file_path);
}

TEST(VpsDemoCodecTest, ScanFileReplyRoundTrip) {
    ScanFileReply reply;
    reply.code = 0;
    reply.threat_level = 1;

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileReply>::Encode(reply, &bytes));

    ScanFileReply decoded;
    ASSERT_TRUE(memrpc::CodecTraits<ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded));
    EXPECT_EQ(decoded.code, reply.code);
    EXPECT_EQ(decoded.threat_level, reply.threat_level);
}

TEST(VpsDemoCodecTest, DecodeRejectsTruncatedPayload) {
    // Valid reply encoding is 8 bytes. Provide 4 bytes to force Decode failure.
    std::vector<uint8_t> truncated(4, 0);
    ScanFileReply decoded;
    EXPECT_FALSE(memrpc::CodecTraits<ScanFileReply>::Decode(truncated.data(), truncated.size(), &decoded));
}

}  // namespace vpsdemo
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_codec_test`

Expected: FAIL because target not defined in CMake.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt` under `if (VPSDEMO_ENABLE_TESTS)`:

```cmake
  add_executable(vpsdemo_codec_test tests/vpsdemo_codec_test.cpp)
  target_link_libraries(vpsdemo_codec_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_codec_test COMMAND vpsdemo_codec_test)
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target vpsdemo_codec_test`

Expected: build succeeds.

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_codec_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vpsdemo_codec_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo codec unit tests"
```

---

### Task 2: Add sample rules unit tests

**Files:**
- Create: `demo/vpsdemo/tests/vpsdemo_sample_rules_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Create `demo/vpsdemo/tests/vpsdemo_sample_rules_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "vpsdemo_sample_rules.h"

namespace vpsdemo {

TEST(VpsSampleRulesTest, DetectsCrashKeyword) {
    auto behavior = EvaluateSamplePath("/data/crash_sample.apk");
    EXPECT_TRUE(behavior.shouldCrash);
}

TEST(VpsSampleRulesTest, DetectsVirusKeywords) {
    EXPECT_EQ(EvaluateSamplePath("/data/virus.apk").threatLevel, 1);
    EXPECT_EQ(EvaluateSamplePath("/data/eicar.txt").threatLevel, 1);
}

TEST(VpsSampleRulesTest, SleepSamplesAreThreats) {
    auto behavior = EvaluateSamplePath("/data/sleep10.bin");
    EXPECT_EQ(behavior.threatLevel, 1);
    EXPECT_EQ(behavior.sleepMs, 10u);
}

TEST(VpsSampleRulesTest, SleepParsingRejectsNonDigit) {
    auto behavior = EvaluateSamplePath("/data/sleepX.bin");
    EXPECT_EQ(behavior.sleepMs, 0u);
}

TEST(VpsSampleRulesTest, SleepCappedAtMax) {
    auto behavior = EvaluateSamplePath("/data/sleep999999.bin");
    EXPECT_EQ(behavior.sleepMs, 5000u);
}

}  // namespace vpsdemo
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_sample_rules_test`

Expected: FAIL because target not defined in CMake.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt` under `if (VPSDEMO_ENABLE_TESTS)`:

```cmake
  add_executable(vpsdemo_sample_rules_test tests/vpsdemo_sample_rules_test.cpp)
  target_link_libraries(vpsdemo_sample_rules_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_sample_rules_test COMMAND vpsdemo_sample_rules_test)
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target vpsdemo_sample_rules_test`

Expected: build succeeds.

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_sample_rules_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vpsdemo_sample_rules_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo sample rules tests"
```

---

### Task 3: Add health snapshot unit tests

**Files:**
- Create: `demo/vpsdemo/tests/vpsdemo_health_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Create `demo/vpsdemo/tests/vpsdemo_health_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "vpsdemo_service.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

TEST(VpsHealthTest, SnapshotIdleDefaults) {
    VpsDemoService service;
    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.in_flight, 0u);
    EXPECT_EQ(snapshot.current_task, "idle");
    EXPECT_EQ(snapshot.last_task_age_ms, 0u);
}

TEST(VpsHealthTest, SnapshotUpdatesAfterScan) {
    VpsDemoService service;
    service.Initialize();

    ScanFileRequest req;
    req.file_path = "/data/virus.apk";

    auto reply = service.ScanFile(req);
    EXPECT_EQ(reply.code, 0);

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_EQ(snapshot.in_flight, 0u);
    EXPECT_EQ(snapshot.current_task, "idle");
}

TEST(VpsHealthTest, InFlightAndAgeDuringScan) {
    VpsDemoService service;
    service.Initialize();

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        ScanFileRequest req;
        req.file_path = "/data/sleep50.bin";
        started.store(true);
        (void)service.ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto snapshot = service.GetHealthSnapshot();
    EXPECT_GE(snapshot.in_flight, 1u);
    EXPECT_NE(snapshot.current_task, "idle");

    worker.join();
}

}  // namespace vpsdemo
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_health_test`

Expected: FAIL because target not defined in CMake.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt` under `if (VPSDEMO_ENABLE_TESTS)`:

```cmake
  add_executable(vpsdemo_health_test tests/vpsdemo_health_test.cpp)
  target_link_libraries(vpsdemo_health_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_health_test COMMAND vpsdemo_health_test)
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target vpsdemo_health_test`

Expected: build succeeds.

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_health_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vpsdemo_health_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo health snapshot tests"
```

---

### Task 4: Add policy/timeout unit test (exec timeout triggers onFailure)

**Files:**
- Create: `demo/vpsdemo/tests/vpsdemo_policy_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Create `demo/vpsdemo/tests/vpsdemo_policy_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

TEST(VpsPolicyTest, ExecTimeoutTriggersOnFailure) {
    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles unused{};
    ASSERT_EQ(bootstrap->OpenSession(unused), memrpc::StatusCode::Ok);

    memrpc::RpcServer server(bootstrap->serverHandles());
    memrpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<memrpc::Opcode>(DemoOpcode::ScanFile),
        [](const ScanFileRequest& req) {
            // Force exec timeout by sleeping longer than client timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ScanFileReply reply;
            reply.code = 0;
            reply.threat_level = 0;
            (void)req;
            return reply;
        });
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    std::atomic<bool> failureCalled{false};
    memrpc::RpcClient client(bootstrap);

    memrpc::RecoveryPolicy policy;
    policy.onFailure = [&](const memrpc::RpcFailure& failure) {
        if (failure.status == memrpc::StatusCode::ExecTimeout) {
            failureCalled.store(true);
        }
        return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    ScanFileRequest req;
    req.file_path = "/data/sleep50.bin";

    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(DemoOpcode::ScanFile);
    call.exec_timeout_ms = 5;  // short timeout
    memrpc::CodecTraits<ScanFileRequest>::Encode(req, &call.payload);

    auto future = client.InvokeAsync(call);
    memrpc::RpcReply reply;
    auto status = future.Wait(&reply);

    EXPECT_EQ(status, memrpc::StatusCode::ExecTimeout);
    EXPECT_TRUE(failureCalled.load());

    client.Shutdown();
    server.Stop();
}

}  // namespace vpsdemo
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_policy_test`

Expected: FAIL because target not defined in CMake.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt` under `if (VPSDEMO_ENABLE_TESTS)`:

```cmake
  add_executable(vpsdemo_policy_test tests/vpsdemo_policy_test.cpp)
  target_link_libraries(vpsdemo_policy_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_policy_test COMMAND vpsdemo_policy_test)
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target vpsdemo_policy_test`

Expected: build succeeds.

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_policy_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vpsdemo_policy_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo policy timeout test"
```

---

### Task 5: Expand heartbeat test assertions

**Files:**
- Modify: `demo/vpsdemo/tests/vps_heartbeat_test.cpp`

**Step 1: Write the failing test**

Extend `OkAfterOpenSession` to assert `in_flight`, `last_task_age_ms` defaults and ensure `version` field is set:

```cpp
    EXPECT_EQ(reply.version, 1u);
    EXPECT_EQ(reply.in_flight, 0u);
    EXPECT_EQ(reply.last_task_age_ms, 0u);
```

Add a new case for in-flight scan:

```cpp
TEST(VpsHeartbeatTest, HeartbeatShowsInFlight) {
    VirusExecutorService service;
    service.OnStart();

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), memrpc::StatusCode::Ok);

    std::atomic<bool> started{false};
    std::thread worker([&]() {
        vpsdemo::ScanFileRequest req;
        req.file_path = "/data/sleep50.bin";
        started.store(true);
        (void)service.ScanFile(req);
    });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    VesHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_GE(reply.in_flight, 1u);
    EXPECT_STRNE(reply.current_task, "idle");

    worker.join();
    service.CloseSession();
    service.OnStop();
}
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_heartbeat_test`

Expected: build fails until includes and threading headers are added.

**Step 3: Write minimal implementation**

Add missing includes in `demo/vpsdemo/tests/vps_heartbeat_test.cpp`:

```cpp
#include <atomic>
#include <chrono>
#include <thread>
```

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_heartbeat_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vps_heartbeat_test.cpp
git commit -m "feat: expand vpsdemo heartbeat tests"
```

---

### Task 6: Add crash-recovery integration test

**Files:**
- Create: `demo/vpsdemo/tests/vpsdemo_crash_recovery_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Create `demo/vpsdemo/tests/vpsdemo_crash_recovery_test.cpp` (adapted from `vpsdemo_dt_crash_recovery.cpp` into a bounded gtest):

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "iservice_registry.h"
#include "registry_backend.h"
#include "registry_server.h"
#include "vps_bootstrap_interface.h"
#include "vps_client.h"
#include "vpsdemo_types.h"

namespace {

const std::string REGISTRY_SOCKET = "/tmp/vpsdemo_it_registry.sock";
const std::string SERVICE_SOCKET = "/tmp/vpsdemo_it_service.sock";

std::mutex g_engine_mutex;
pid_t g_engine_pid = -1;

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

std::string EnginePathFromArgv(const char* argv0) {
    std::string dir = ".";
    std::string a0(argv0);
    auto pos = a0.rfind('/');
    if (pos != std::string::npos) {
        dir = a0.substr(0, pos);
    }
    return dir + "/vpsdemo_engine_sa";
}

}  // namespace

TEST(VpsCrashRecoveryTest, CrashThenRecover) {
    const std::string enginePath = EnginePathFromArgv(::testing::UnitTest::GetInstance()->original_working_dir().c_str());

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

    ASSERT_TRUE(registry.Start());

    auto backend = std::make_shared<vpsdemo::RegistryBackend>(REGISTRY_SOCKET);
    OHOS::SystemAbilityManagerClient::GetInstance().SetBackend(backend);
    vpsdemo::VpsClient::RegisterProxyFactory();

    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine_pid = SpawnEngine(enginePath);
    }
    ASSERT_GT(g_engine_pid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    auto remote = sam->LoadSystemAbility(vpsdemo::VPS_BOOTSTRAP_SA_ID, 5000);
    ASSERT_NE(remote, nullptr);

    std::atomic<int> engineRestarts{0};

    auto client = std::make_unique<vpsdemo::VpsClient>(remote);
    client->SetEngineRestartCallback([&]() {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine_pid > 0) {
            int status = 0;
            waitpid(g_engine_pid, &status, WNOHANG);
            g_engine_pid = -1;
        }
        g_engine_pid = SpawnEngine(enginePath);
        if (g_engine_pid > 0) {
            engineRestarts++;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    ASSERT_EQ(client->Init(), memrpc::StatusCode::Ok);

    vpsdemo::ScanFileReply reply;
    ASSERT_EQ(client->ScanFile("/data/clean.apk", &reply), memrpc::StatusCode::Ok);

    // Crash request
    (void)client->ScanFile("/data/crash.apk", &reply);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engineRestarts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GT(engineRestarts.load(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_EQ(client->ScanFile("/data/clean_after.apk", &reply), memrpc::StatusCode::Ok);

    client->Shutdown();
    registry.Stop();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        KillAndWait(g_engine_pid);
        g_engine_pid = -1;
    }
}
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target vpsdemo_crash_recovery_test`

Expected: FAIL because target not defined in CMake.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt` under `if (VPSDEMO_ENABLE_TESTS)`:

```cmake
  add_executable(vpsdemo_crash_recovery_test tests/vpsdemo_crash_recovery_test.cpp)
  target_link_libraries(vpsdemo_crash_recovery_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_crash_recovery_test COMMAND vpsdemo_crash_recovery_test)
```

Optionally label as `dt`:

```cmake
  set_tests_properties(vpsdemo_crash_recovery_test PROPERTIES LABELS "dt")
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target vpsdemo_crash_recovery_test`

Expected: build succeeds.

Run: `ctest --test-dir build --output-on-failure -R vpsdemo_crash_recovery_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/vpsdemo_crash_recovery_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo crash recovery test"
```

---

### Task 7: Add vpsdemo codec fuzz target

**Files:**
- Create: `demo/vpsdemo/tests/fuzz/vpsdemo_codec_fuzz.cpp`
- Create: `demo/vpsdemo/tests/fuzz/CMakeLists.txt`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test (fuzz target)**

Create `demo/vpsdemo/tests/fuzz/vpsdemo_codec_fuzz.cpp`:

```cpp
#include <cstddef>
#include <cstdint>
#include <string>

#include "vpsdemo_codec.h"
#include "vpsdemo_sample_rules.h"
#include "vpsdemo_types.h"

namespace {

std::string SanitizePath(std::string path) {
    // Strip "crash" to avoid abort().
    const std::string kCrash = "crash";
    for (;;) {
        auto pos = path.find(kCrash);
        if (pos == std::string::npos) break;
        path.erase(pos, kCrash.size());
    }

    // Clamp sleep to 0 by replacing "sleep" digits with "sleep0".
    const std::string kSleep = "sleep";
    auto pos = path.find(kSleep);
    if (pos != std::string::npos) {
        size_t i = pos + kSleep.size();
        while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i]))) {
            path.erase(i, 1);
        }
        path.insert(pos + kSleep.size(), "0");
    }
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    vpsdemo::ScanFileRequest req;
    if (!memrpc::CodecTraits<vpsdemo::ScanFileRequest>::Decode(data, size, &req)) {
        return 0;
    }

    req.file_path = SanitizePath(std::move(req.file_path));
    (void)vpsdemo::EvaluateSamplePath(req.file_path);

    vpsdemo::ScanFileReply reply;
    reply.code = 0;
    reply.threat_level = 0;

    std::vector<uint8_t> bytes;
    if (memrpc::CodecTraits<vpsdemo::ScanFileReply>::Encode(reply, &bytes)) {
        vpsdemo::ScanFileReply decoded;
        (void)memrpc::CodecTraits<vpsdemo::ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded);
    }

    return 0;
}
```

Create `demo/vpsdemo/tests/fuzz/CMakeLists.txt`:

```cmake
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "VPSDEMO_ENABLE_FUZZ requires Clang")
endif()

add_executable(vpsdemo_codec_fuzz vpsdemo_codec_fuzz.cpp)
set_target_properties(vpsdemo_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

target_link_libraries(vpsdemo_codec_fuzz PRIVATE vpsdemo_lib)

target_compile_options(vpsdemo_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

target_link_options(vpsdemo_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME vpsdemo_codec_fuzz_smoke COMMAND vpsdemo_codec_fuzz -runs=1)
set_tests_properties(vpsdemo_codec_fuzz_smoke PROPERTIES LABELS "fuzz")
```

**Step 2: Run test to verify it fails**

Run: `cmake -S demo/vpsdemo -B build_vps_fuzz -DVPSDEMO_ENABLE_TESTS=ON -DVPSDEMO_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++`

Expected: configure fails because `VPSDEMO_ENABLE_FUZZ` is not declared or subdirectory missing.

**Step 3: Write minimal implementation**

Edit `demo/vpsdemo/CMakeLists.txt`:

- Add option:

```cmake
option(VPSDEMO_ENABLE_FUZZ "Build vpsdemo fuzz targets" OFF)
```

- Under the tests block, add:

```cmake
if (VPSDEMO_ENABLE_FUZZ)
  add_subdirectory(tests/fuzz)
endif()
```

**Step 4: Run test to verify it passes**

Run:

```
cmake -S demo/vpsdemo -B build_vps_fuzz -DVPSDEMO_ENABLE_TESTS=ON -DVPSDEMO_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_vps_fuzz --target vpsdemo_codec_fuzz
ctest --test-dir build_vps_fuzz -L fuzz --output-on-failure
```

Expected: fuzz smoke test PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/tests/fuzz/vpsdemo_codec_fuzz.cpp demo/vpsdemo/tests/fuzz/CMakeLists.txt demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo codec fuzz target"
```

---

### Task 8: Optional test organization cleanups

**Files:**
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Add labels to integration tests**

Add label for crash recovery to avoid default runs:

```cmake
set_tests_properties(vpsdemo_crash_recovery_test PROPERTIES LABELS "dt")
```

**Step 2: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -L dt`

Expected: PASS.

**Step 3: Commit**

```bash
git add demo/vpsdemo/CMakeLists.txt
git commit -m "feat: label vpsdemo integration tests"
```

---

## Rollup Verification

- Unit tests:

```
cmake -S demo/vpsdemo -B build_vps -DVPSDEMO_ENABLE_TESTS=ON
cmake --build build_vps
ctest --test-dir build_vps --output-on-failure
```

- Fuzz smoke:

```
cmake -S demo/vpsdemo -B build_vps_fuzz -DVPSDEMO_ENABLE_TESTS=ON -DVPSDEMO_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_vps_fuzz
ctest --test-dir build_vps_fuzz -L fuzz --output-on-failure
```
