# CMake To GN Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a GN-based build description that mirrors the current CMake target graph for `memrpc`, `virus_executor_service`, and required third-party targets.

**Architecture:** Introduce a small shared GN config layer for compiler flags and build args, then translate each module into local `BUILD.gn` targets that preserve current source grouping and dependency direction. Keep the GN graph focused on buildable libraries, executables, and test binaries; do not attempt to reimplement CTest labeling or timeout metadata inside GN.

**Tech Stack:** GN, Clang, Ninja, C++17, GoogleTest, POSIX pthread/rt system libraries

---

### Task 1: Add GN workspace entrypoints and shared configs

**Files:**
- Create: `.gn`
- Create: `BUILD.gn`
- Create: `gn/BUILDCONFIG.gn`
- Create: `gn/config/compiler.gni`
- Create: `gn/config/test.gni`

**Step 1: Write the failing generation check**

Run: `gn gen out/gn`
Expected: FAIL because `.gn` and root `BUILD.gn` do not exist.

**Step 2: Write minimal GN workspace files**

Add a root `.gn` that points `buildconfig` at `//gn/BUILDCONFIG.gn`.

Add a root `BUILD.gn` that defines:

```gn
group("default") {
  deps = [
    "//memrpc:memrpc",
    "//virus_executor_service:virus_executor_service",
  ]
}
```

Add shared args/config in `gn/config/compiler.gni` for:
- `memrpc_enable_tests`
- `memrpc_enable_dt_tests`
- `memrpc_enable_stress_tests`
- `memrpc_enable_fuzz`
- `memrpc_enable_strict_warnings`
- `memrpc_warnings_as_errors`
- `memrpc_enable_asan`
- `memrpc_enable_ubsan`
- `memrpc_enable_tsan`

Add reusable test templates in `gn/config/test.gni` for plain gtest executables.

**Step 3: Run generation again**

Run: `gn gen out/gn`
Expected: FAIL later on unresolved module targets, proving root config is wired.

### Task 2: Translate `memrpc` library and tests

**Files:**
- Create: `memrpc/BUILD.gn`

**Step 1: Define the core library**

Translate `memrpc/src/CMakeLists.txt` into:

```gn
source_set("memrpc_core") {
  sources = [
    "src/client/rpc_client.cpp",
    "src/server/rpc_server.cpp",
    "src/bootstrap/dev_bootstrap.cpp",
    "src/bootstrap/sa_bootstrap.cpp",
    "src/core/byte_reader.cpp",
    "src/core/byte_writer.cpp",
    "src/core/log.cpp",
    "src/core/session.cpp",
  ]
  include_dirs = [
    "include",
    "src",
    "//include",
  ]
  libs = [
    "pthread",
    "rt",
  ]
}

group("memrpc") {
  public_deps = [ ":memrpc_core" ]
}
```

**Step 2: Translate tests behind args**

Add a small GN template usage for each file in `memrpc/tests/`, including the watchdog test define:

```gn
memrpc_gtest("memrpc_rpc_client_timeout_watchdog_test") {
  sources = [ "tests/rpc_client_timeout_watchdog_test.cpp" ]
  defines = [ "MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS=20" ]
}
```

Gate DT tests with `memrpc_enable_dt_tests`.

**Step 3: Run generation**

Run: `gn gen out/gn`
Expected: FAIL only on downstream targets not yet translated.

### Task 3: Translate third-party support targets

**Files:**
- Modify: `third_party/ohos_sa_mock/BUILD.gn`
- Modify: `third_party/googletest/BUILD.gn`

**Step 1: Bring `ohos_sa_mock` GN target up to CMake parity**

Update `third_party/ohos_sa_mock/BUILD.gn` so `ohos_sa_mock` includes:
- `src/iremote_broker_registry.cpp`
- `src/scm_rights.cpp`
- `src/mock_service_socket.cpp`

Add required public include dirs and `pthread` linkage.

**Step 2: Keep GoogleTest consumption simple**

Ensure `third_party/googletest/BUILD.gn` exposes a target that test templates can depend on, with:

```gn
libs = [ "gtest", "gtest_main", "pthread" ]
```

**Step 3: Run generation**

Run: `gn gen out/gn`
Expected: FAIL only on `virus_executor_service` targets if any remain unresolved.

### Task 4: Translate `virus_executor_service` libraries, apps, tests, and fuzz targets

**Files:**
- Create: `virus_executor_service/BUILD.gn`
- Optionally create: `virus_executor_service/tests/fuzz/BUILD.gn`

**Step 1: Define the module libraries**

Translate the CMake libraries into GN `source_set` targets:
- `virus_executor_service_transport`
- `virus_executor_service_ves`
- `virus_executor_service_client_lib`
- `virus_executor_service_testkit`
- `virus_executor_service_service`
- `virus_executor_service_lib`
- `virus_executor_service_testkit_stress_config`

All app/module targets should expose:

```gn
include_dirs = [
  "include",
  "//include",
  "//memrpc/include",
]
```

and depend on `//memrpc:memrpc_core` plus `//third_party/ohos_sa_mock:ohos_sa_mock` where needed.

**Step 2: Define the executables**

Translate these app targets:
- `virus_executor_service_supervisor`
- `virus_executor_service`
- `virus_executor_service_client`
- `virus_executor_service_stress_client`
- `virus_executor_service_dt_crash_recovery`
- `virus_executor_service_testkit_stress_runner`

**Step 3: Define unit/DT/stress/fuzz tests behind args**

Use the shared gtest template for all current CMake test executables. Gate:
- regular tests with `memrpc_enable_tests`
- DT tests with `memrpc_enable_dt_tests`
- stress binaries with `memrpc_enable_stress_tests || memrpc_enable_tests`
- fuzz binaries with `memrpc_enable_fuzz`

For fuzz targets, carry over the sanitizer flags in target-local configs.

**Step 4: Run generation**

Run: `gn gen out/gn`
Expected: PASS.

### Task 5: Verify representative GN builds

**Files:**
- Modify: any GN file touched above if verification exposes missing deps/includes

**Step 1: Build representative non-test targets**

Run: `ninja -C out/gn memrpc_core virus_executor_service`
Expected: PASS.

**Step 2: Build representative tests**

Run: `ninja -C out/gn memrpc_smoke_test virus_executor_service_codec_test`
Expected: PASS.

**Step 3: Build a fuzz target when enabled**

Run: `gn gen out/gn_fuzz --args='memrpc_enable_tests=true memrpc_enable_fuzz=true memrpc_enable_asan=true memrpc_enable_ubsan=true'`

Then run:

```bash
ninja -C out/gn_fuzz virus_executor_service_codec_fuzz
```

Expected: PASS if local `gn` toolchain and system fuzz runtime are available; otherwise document the local tool/runtime gap precisely.
