# CMake to GN Parity Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add GN build files that match CMake targets and options while keeping CMake intact.

**Architecture:** Use root-level configs and args, plus per-directory `BUILD.gn` files aligned to CMake subdirectories. Root `BUILD.gn` aggregates targets into groups and gates tests/fuzz/stress via args.

**Tech Stack:** GN/Ninja, C++17, GoogleTest.

---

### Task 0: Add GN bootstrap files

**Files:**
- Create: `.gn`
- Create: `build/config/BUILDCONFIG.gn`
- Create: `build/config/BUILD.gn`
- Create: `build/toolchain/BUILD.gn`
- Create: `third_party/googletest/BUILD.gn`
- Modify: `BUILD.gn`

**Step 1: Add `.gn`**

Create `.gn`:

```gn
buildconfig = "//build/config/BUILDCONFIG.gn"

default_args = {
  is_debug = true
  is_clang = true
}
```

**Step 2: Add `build/config/BUILDCONFIG.gn`**

```gn
declare_args() {
  is_debug = true
  is_clang = true
}

if (is_clang) {
  default_toolchain = "//build/toolchain:clang"
} else {
  default_toolchain = "//build/toolchain:gcc"
}

set_default_toolchain(default_toolchain)
```

**Step 3: Add `build/config/BUILD.gn`**

```gn
config("default_compiler_config") {
  cflags = []
  cflags_cc = [ "-std=c++17" ]
  if (is_debug) {
    cflags += [ "-g", "-O0" ]
  } else {
    cflags += [ "-O2" ]
  }
}
```

**Step 4: Wire defaults to `//build/config:default_compiler_config`**

```gn
set_defaults("executable") {
  configs += [ "//build/config:default_compiler_config" ]
}

set_defaults("static_library") {
  configs += [ "//build/config:default_compiler_config" ]
}

set_defaults("source_set") {
  configs += [ "//build/config:default_compiler_config" ]
}
```

**Step 5: Add `build/toolchain/BUILD.gn`**

```gn
toolchain("clang") {
  lib_prefix = "lib"
  lib_extension = "a"
  solib_prefix = "lib"
  solib_extension = "so"

  tool("cc") {
    command = "clang -MMD -MF $out.d $defines $include_dirs $cflags $cflags_c -c $in -o $out"
    depfile = "$out.d"
    depsformat = "gcc"
    description = "CC $out"
  }

  tool("cxx") {
    command = "clang++ -MMD -MF $out.d $defines $include_dirs $cflags $cflags_cc -c $in -o $out"
    depfile = "$out.d"
    depsformat = "gcc"
    description = "CXX $out"
  }

  tool("alink") {
    command = "rm -f $out && ar rcs $out $in"
    description = "AR $out"
    outputs = [ "$out" ]
  }

  tool("solink") {
    command = "clang++ -shared $ldflags $lib_dirs -o $out $in $libs"
    description = "SOLINK $out"
    outputs = [ "$out" ]
  }

  tool("link") {
    command = "clang++ $ldflags $lib_dirs -o $out $in $libs"
    description = "LINK $out"
    outputs = [ "$out" ]
  }

  tool("stamp") {
    command = "touch $out"
  }

  tool("copy") {
    command = "cp -f $in $out"
  }
}

toolchain("gcc") {
  lib_prefix = "lib"
  lib_extension = "a"
  solib_prefix = "lib"
  solib_extension = "so"

  tool("cc") {
    command = "gcc -MMD -MF $out.d $defines $include_dirs $cflags $cflags_c -c $in -o $out"
    depfile = "$out.d"
    depsformat = "gcc"
    description = "CC $out"
  }

  tool("cxx") {
    command = "g++ -MMD -MF $out.d $defines $include_dirs $cflags $cflags_cc -c $in -o $out"
    depfile = "$out.d"
    depsformat = "gcc"
    description = "CXX $out"
  }

  tool("alink") {
    command = "rm -f $out && ar rcs $out $in"
    description = "AR $out"
    outputs = [ "$out" ]
  }

  tool("solink") {
    command = "g++ -shared $ldflags $lib_dirs -o $out $in $libs"
    description = "SOLINK $out"
    outputs = [ "$out" ]
  }

  tool("link") {
    command = "g++ $ldflags $lib_dirs -o $out $in $libs"
    description = "LINK $out"
    outputs = [ "$out" ]
  }

  tool("stamp") {
    command = "touch $out"
  }

  tool("copy") {
    command = "cp -f $in $out"
  }
}
```

**Step 6: Add a GN shim for system GTest**

Create `third_party/googletest/BUILD.gn`:

```gn
config("gtest_config") {
  libs = [
    "gtest",
    "gtest_main",
    "pthread",
  ]
}

source_set("gtest_main") {
  public_configs = [ ":gtest_config" ]
}
```

**Step 7: Allow GN config files under `build/`**

Update `.gitignore` to unignore the GN bootstrap files:

```gitignore
!build/
!build/config/
!build/config/BUILDCONFIG.gn
!build/toolchain/
!build/toolchain/BUILD.gn
```

**Step 8: Import memrpc args in root `BUILD.gn`**

Add to the top of `BUILD.gn`:

```gn
import("memrpc.gni")
```

**Step 9: Commit**

```bash
git add .gn build/config/BUILDCONFIG.gn build/config/BUILD.gn build/toolchain/BUILD.gn third_party/googletest/BUILD.gn BUILD.gn .gitignore
git commit -m "feat: add gn bootstrap toolchain"
```

### Task 1: Extend GN build args

**Files:**
- Modify: `memrpc.gni`

**Step 1: Confirm new args are currently undefined**

Run: `gn gen out/gn --args='memrpc_enable_stress_tests=true'`
Expected: FAIL with an error like `Unknown arg: memrpc_enable_stress_tests`.

**Step 2: Add the missing args**

Update `memrpc.gni`:

```gn
declare_args() {
  memrpc_enable_tests = true
  memrpc_enable_stress_tests = false
  memrpc_enable_fuzz = false
  memrpc_enable_dt_tests = true
  memrpc_gtest_main_target = "//third_party/googletest:gtest_main"
}
```

**Step 3: Re-run gen to ensure args are accepted**

Run: `gn gen out/gn --args='memrpc_enable_stress_tests=true'`
Expected: FAIL for unrelated reasons (missing targets/files) until later tasks are done, but no “unknown arg” error.

**Step 4: Commit**

```bash
git add memrpc.gni
git commit -m "feat: add gn args for memrpc toggles"
```

---

### Task 2: Define core libraries in `src/BUILD.gn`

**Files:**
- Create: `src/BUILD.gn`

**Step 1: Add `memrpc` and `minirpc_demo` targets**

Create `src/BUILD.gn`:

```gn
source_set("memrpc") {
  sources = [
    "bootstrap/posix_demo_bootstrap.cpp",
    "bootstrap/sa_bootstrap.cpp",
    "client/rpc_client.cpp",
    "core/byte_reader.cpp",
    "core/byte_writer.cpp",
    "core/log.cpp",
    "core/session.cpp",
    "core/slot_pool.cpp",
    "server/rpc_server.cpp",
  ]
  public_configs = [ "//:memrpc_config" ]
}

source_set("minirpc_demo") {
  sources = [
    "apps/minirpc/child/minirpc_service.cpp",
    "apps/minirpc/parent/minirpc_async_client.cpp",
    "apps/minirpc/parent/minirpc_client.cpp",
  ]
  public_deps = [ ":memrpc" ]
  public_configs = [ "//:memrpc_config" ]
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn`
Expected: May still FAIL until root `BUILD.gn` is updated, but this file should parse cleanly.

**Step 3: Commit**

```bash
git add src/BUILD.gn
git commit -m "feat: add gn targets for memrpc libs"
```

---

### Task 3: Add demo executable in `demo/BUILD.gn`

**Files:**
- Create: `demo/BUILD.gn`

**Step 1: Add demo target**

```gn
executable("memrpc_minirpc_demo") {
  sources = [ "minirpc_demo_main.cpp" ]
  deps = [ "//src:minirpc_demo" ]
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn`
Expected: May still FAIL until root `BUILD.gn` is updated, but this file should parse cleanly.

**Step 3: Commit**

```bash
git add demo/BUILD.gn
git commit -m "feat: add gn demo target"
```

---

### Task 4: Add memrpc tests in `tests/memrpc/BUILD.gn`

**Files:**
- Create: `tests/memrpc/BUILD.gn`

**Step 1: Add test template and targets**

```gn
template("memrpc_test") {
  executable(target_name) {
    testonly = true
    sources = invoker.sources
    deps = [
      "//src:memrpc",
      memrpc_gtest_main_target,
    ]
    configs += [ "//:memrpc_test_config" ]
    if (defined(invoker.defines)) {
      defines = invoker.defines
    }
  }
}

memrpc_test("memrpc_smoke_test") { sources = [ "smoke_test.cpp" ] }
memrpc_test("memrpc_types_test") { sources = [ "types_test.cpp" ] }
memrpc_test("memrpc_api_headers_test") { sources = [ "api_headers_test.cpp" ] }
memrpc_test("memrpc_framework_split_headers_test") { sources = [ "framework_split_headers_test.cpp" ] }
memrpc_test("memrpc_log_test") { sources = [ "log_test.cpp" ] }
memrpc_test("memrpc_protocol_layout_test") { sources = [ "protocol_layout_test.cpp" ] }
memrpc_test("memrpc_byte_codec_test") { sources = [ "byte_codec_test.cpp" ] }
memrpc_test("memrpc_slot_pool_test") { sources = [ "slot_pool_test.cpp" ] }
memrpc_test("memrpc_session_test") { sources = [ "session_test.cpp" ] }
memrpc_test("memrpc_build_config_test") { sources = [ "build_config_test.cpp" ] }
memrpc_test("memrpc_rpc_client_api_test") { sources = [ "rpc_client_api_test.cpp" ] }
memrpc_test("memrpc_rpc_server_api_test") { sources = [ "rpc_server_api_test.cpp" ] }
memrpc_test("memrpc_sa_bootstrap_stub_test") { sources = [ "sa_bootstrap_stub_test.cpp" ] }
memrpc_test("memrpc_replay_classifier_test") { sources = [ "replay_classifier_test.cpp" ] }
memrpc_test("memrpc_rpc_client_timeout_watchdog_test") {
  sources = [ "rpc_client_timeout_watchdog_test.cpp" ]
  defines = [ "MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS=20" ]
}
memrpc_test("memrpc_rpc_client_idle_callback_test") { sources = [ "rpc_client_idle_callback_test.cpp" ] }
memrpc_test("memrpc_rpc_server_executor_test") { sources = [ "rpc_server_executor_test.cpp" ] }
memrpc_test("memrpc_typed_future_test") { sources = [ "typed_future_test.cpp" ] }
memrpc_test("memrpc_engine_death_handler_test") { sources = [ "engine_death_handler_test.cpp" ] }
memrpc_test("memrpc_rpc_client_recovery_policy_test") { sources = [ "rpc_client_recovery_policy_test.cpp" ] }

if (memrpc_enable_dt_tests) {
  memrpc_test("memrpc_dt_stability_test") { sources = [ "dt_stability_test.cpp" ] }
  memrpc_test("memrpc_dt_perf_test") { sources = [ "dt_perf_test.cpp" ] }
}

group("memrpc_tests") {
  deps = [
    ":memrpc_smoke_test",
    ":memrpc_types_test",
    ":memrpc_api_headers_test",
    ":memrpc_framework_split_headers_test",
    ":memrpc_log_test",
    ":memrpc_protocol_layout_test",
    ":memrpc_byte_codec_test",
    ":memrpc_slot_pool_test",
    ":memrpc_session_test",
    ":memrpc_build_config_test",
    ":memrpc_rpc_client_api_test",
    ":memrpc_rpc_server_api_test",
    ":memrpc_sa_bootstrap_stub_test",
    ":memrpc_replay_classifier_test",
    ":memrpc_rpc_client_timeout_watchdog_test",
    ":memrpc_rpc_client_idle_callback_test",
    ":memrpc_rpc_server_executor_test",
    ":memrpc_typed_future_test",
    ":memrpc_engine_death_handler_test",
    ":memrpc_rpc_client_recovery_policy_test",
  ]
  if (memrpc_enable_dt_tests) {
    deps += [
      ":memrpc_dt_stability_test",
      ":memrpc_dt_perf_test",
    ]
  }
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn`
Expected: May still FAIL until root `BUILD.gn` is updated, but this file should parse cleanly.

**Step 3: Commit**

```bash
git add tests/memrpc/BUILD.gn
git commit -m "feat: add gn memrpc test targets"
```

---

### Task 5: Add minirpc tests and stress targets

**Files:**
- Create: `tests/apps/minirpc/BUILD.gn`

**Step 1: Add minirpc test template and targets**

```gn
template("minirpc_test") {
  executable(target_name) {
    testonly = true
    sources = invoker.sources
    deps = [
      "//src:minirpc_demo",
      memrpc_gtest_main_target,
    ]
    configs += [ "//:memrpc_test_config" ]
  }
}

minirpc_test("memrpc_minirpc_headers_test") { sources = [ "minirpc_headers_test.cpp" ] }
minirpc_test("memrpc_minirpc_codec_test") { sources = [ "minirpc_codec_test.cpp" ] }
minirpc_test("memrpc_minirpc_service_test") { sources = [ "minirpc_service_test.cpp" ] }
minirpc_test("memrpc_minirpc_client_test") { sources = [ "minirpc_client_test.cpp" ] }
minirpc_test("memrpc_minirpc_throughput_test") { sources = [ "minirpc_throughput_test.cpp" ] }
minirpc_test("memrpc_minirpc_baseline_test") { sources = [ "minirpc_baseline_test.cpp" ] }
minirpc_test("memrpc_minirpc_latency_test") { sources = [ "minirpc_latency_test.cpp" ] }
minirpc_test("memrpc_minirpc_async_pipeline_test") { sources = [ "minirpc_async_pipeline_test.cpp" ] }
minirpc_test("memrpc_minirpc_backpressure_test") { sources = [ "minirpc_backpressure_test.cpp" ] }
minirpc_test("memrpc_minirpc_dfx_test") {
  sources = [
    "minirpc_dfx_test.cpp",
    "minirpc_resilient_invoker.cpp",
  ]
}

if (memrpc_enable_dt_tests) {
  minirpc_test("memrpc_minirpc_dt_stability_test") { sources = [ "minirpc_dt_stability_test.cpp" ] }
  minirpc_test("memrpc_minirpc_dt_perf_test") { sources = [ "minirpc_dt_perf_test.cpp" ] }
}

if (memrpc_enable_stress_tests) {
  source_set("minirpc_stress_config") {
    sources = [ "minirpc_stress_config.cpp" ]
    deps = [ "//src:minirpc_demo" ]
    public_configs = [ "//:memrpc_config" ]
    include_dirs = [ "//tests" ]
  }

  executable("memrpc_minirpc_stress_config_test") {
    testonly = true
    sources = [ "minirpc_stress_config_test.cpp" ]
    deps = [
      ":minirpc_stress_config",
      memrpc_gtest_main_target,
    ]
    configs += [ "//:memrpc_test_config" ]
  }

  executable("memrpc_minirpc_stress_runner") {
    testonly = true
    sources = [ "minirpc_stress_runner.cpp" ]
    deps = [
      ":minirpc_stress_config",
      "//src:minirpc_demo",
    ]
    configs += [ "//:memrpc_test_config" ]
  }
}

group("minirpc_tests") {
  deps = [
    ":memrpc_minirpc_headers_test",
    ":memrpc_minirpc_codec_test",
    ":memrpc_minirpc_service_test",
    ":memrpc_minirpc_client_test",
    ":memrpc_minirpc_throughput_test",
    ":memrpc_minirpc_baseline_test",
    ":memrpc_minirpc_latency_test",
    ":memrpc_minirpc_async_pipeline_test",
    ":memrpc_minirpc_backpressure_test",
    ":memrpc_minirpc_dfx_test",
  ]
  if (memrpc_enable_dt_tests) {
    deps += [
      ":memrpc_minirpc_dt_stability_test",
      ":memrpc_minirpc_dt_perf_test",
    ]
  }
  if (memrpc_enable_stress_tests) {
    deps += [
      ":memrpc_minirpc_stress_config_test",
      ":memrpc_minirpc_stress_runner",
    ]
  }
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn`
Expected: May still FAIL until root `BUILD.gn` is updated, but this file should parse cleanly.

**Step 3: Commit**

```bash
git add tests/apps/minirpc/BUILD.gn
git commit -m "feat: add gn minirpc test targets"
```

---

### Task 6: Add fuzz target in `tests/fuzz/BUILD.gn`

**Files:**
- Create: `tests/fuzz/BUILD.gn`

**Step 1: Add fuzz target guarded by arg**

```gn
if (memrpc_enable_fuzz) {
  assert(is_clang, "memrpc_enable_fuzz requires Clang")

  executable("minirpc_codec_fuzz") {
    testonly = true
    sources = [ "minirpc_codec_fuzz.cpp" ]
    deps = [ "//src:minirpc_demo" ]
    configs += [ "//:memrpc_test_config" ]
    cflags_cc = [ "-fsanitize=fuzzer,address,undefined" ]
    ldflags = [ "-fsanitize=fuzzer,address,undefined" ]
  }

  group("fuzz_targets") {
    deps = [ ":minirpc_codec_fuzz" ]
  }
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn-fuzz --args='memrpc_enable_fuzz=true'`
Expected: FAIL if not using Clang (assert), otherwise GN files generate.

**Step 3: Commit**

```bash
git add tests/fuzz/BUILD.gn
git commit -m "feat: add gn fuzz target"
```

---

### Task 7: Update root `BUILD.gn` to wire subtargets

**Files:**
- Modify: `BUILD.gn`

**Step 1: Replace old targets with root configs and groups**

Key changes:
- `import("memrpc.gni")`
- Keep only `memrpc_config` and `memrpc_test_config` at root.
- Remove non-CMake targets (e.g., vps demo) or move them behind a separate arg if still needed later.
- Add groups to aggregate subdirectory targets.

Example structure:

```gn
import("memrpc.gni")

config("memrpc_config") {
  include_dirs = [ "include", "src" ]
  cflags_cc = [ "-std=c++17" ]
}

config("memrpc_test_config") {
  include_dirs = [ "include", "src" ]
  cflags_cc = [ "-std=c++17" ]
}

group("memrpc_libs") {
  deps = [
    "//src:memrpc",
    "//src:minirpc_demo",
  ]
}

group("memrpc_demo") {
  deps = [ "//demo:memrpc_minirpc_demo" ]
}

if (memrpc_enable_tests) {
  group("memrpc_tests") {
    deps = [
      "//tests/memrpc:memrpc_tests",
      "//tests/apps/minirpc:minirpc_tests",
    ]
  }
}

if (memrpc_enable_fuzz) {
  group("memrpc_fuzz") {
    deps = [ "//tests/fuzz:fuzz_targets" ]
  }
}

group("all") {
  deps = [
    ":memrpc_libs",
    ":memrpc_demo",
  ]
  if (memrpc_enable_tests) {
    deps += [ ":memrpc_tests" ]
  }
  if (memrpc_enable_fuzz) {
    deps += [ ":memrpc_fuzz" ]
  }
}
```

**Step 2: Regenerate GN files**

Run: `gn gen out/gn`
Expected: SUCCESS.

**Step 3: Commit**

```bash
git add BUILD.gn
git commit -m "feat: wire gn targets to match cmake"
```

---

### Task 8: Verification builds (base, stress, fuzz)

**Files:**
- None (build outputs only)

**Step 1: Base build with tests enabled**

Run:
- `gn gen out/gn --args='memrpc_enable_tests=true memrpc_enable_dt_tests=true memrpc_enable_stress_tests=false memrpc_enable_fuzz=false'`
- `ninja -C out/gn all`

Expected: Build succeeds.

**Step 2: Stress build**

Run:
- `gn gen out/gn-stress --args='memrpc_enable_tests=true memrpc_enable_stress_tests=true memrpc_enable_dt_tests=true memrpc_enable_fuzz=false'`
- `ninja -C out/gn-stress memrpc_minirpc_stress_runner`

Expected: Build succeeds.

**Step 3: Fuzz build (Clang)**

Run:
- `gn gen out/gn-fuzz --args='memrpc_enable_fuzz=true'`
- `ninja -C out/gn-fuzz minirpc_codec_fuzz`

Expected: Build succeeds when using Clang; fails with a clear assert otherwise.

**Step 4: Commit (verification note)**

If verification scripts/logs are added (not expected), commit them; otherwise no commit needed.
