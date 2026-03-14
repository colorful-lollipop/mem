# ARM64 Concurrency Validation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Validate whether the shared-memory RPC framework and Virus Executor Service have ARM64-specific concurrency or memory-order issues, using an ARM64 simulator first and real ARM64 hardware second.

**Architecture:** Use a two-stage verification strategy. Stage 1 runs the existing repo test suite on an ARM64 simulator to catch architecture-specific correctness bugs, including memory ordering assumptions, atomic usage mistakes, and ARM64-only code paths such as `CpuRelax()`. Stage 2 runs the timing-sensitive, stress, DT, and recovery paths on real ARM64 hardware to catch scheduler, cache, contention, and latency behaviors that a simulator cannot model accurately.

**Tech Stack:** C++17, CMake, clang, ninja, GoogleTest, CTest, shared memory, Unix-domain sockets, ARM64 simulator or VM, ARM64 real device.

---

### Task 1: Establish the x86_64 baseline before any ARM64 run

**Files:**
- Modify: `none`
- Test: `tools/build_and_test.sh`
- Test: `tools/push_gate.sh`

**Step 1: Run the default local baseline**

Run:

```bash
tools/build_and_test.sh --test-regex "memrpc|virus_executor_service"
```

Expected: configure, build, and the targeted test subset pass on the current host.

**Step 2: Run the minimum push gate if the change under investigation touched concurrency-sensitive code**

Run:

```bash
tools/push_gate.sh
```

Expected: gate passes or produces a concrete failing test to compare against ARM64 later.

**Step 3: Record the current flaky or failing tests**

Run:

```bash
ctest --test-dir build_ninja -N
```

Expected: a stable inventory of configured tests exists before moving to ARM64.

**Step 4: Do not start ARM64 analysis until the baseline status is known**

Expected: every later ARM64 failure can be classified as either pre-existing or ARM64-specific.

**Step 5: Commit any preparatory harness-only changes if you had to add them**

```bash
git add <only-harness-or-doc-files>
git commit -m "chore: prepare arm64 concurrency validation baseline"
```

### Task 2: Prepare an ARM64 simulator environment that matches the repo’s runtime needs

**Files:**
- Modify: `none`
- Test: `docs/plans/2026-03-14-arm64-concurrency-validation-plan.md`

**Step 1: Choose a simulator or VM that runs real ARM64 Linux userland**

Preferred options:

```text
QEMU aarch64 VM
Apple Silicon Linux VM
Cloud ARM64 VM
```

Expected: the environment can run child processes, shared memory, eventfd, and Unix-domain sockets.

**Step 2: Verify toolchain availability inside the ARM64 environment**

Run:

```bash
clang --version
cmake --version
ninja --version
```

Expected: all tools exist and report versions without fallback to a different compiler family.

**Step 3: Verify the runtime primitives used by this repo are not blocked**

Run:

```bash
python3 - <<'PY'
import os, socket
fd = os.eventfd(0, os.EFD_NONBLOCK)
name = "/memrpc_arm64_probe"
shm_fd = os.shm_open(name, os.O_CREAT | os.O_RDWR, 0o600)
os.ftruncate(shm_fd, 4096)
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
print("ok")
sock.close()
os.close(fd)
os.close(shm_fd)
os.shm_unlink(name)
PY
```

Expected: prints `ok` and exits cleanly.

**Step 4: Copy or mount the repo into the ARM64 environment**

Expected: the repo is available with write access to the build directory.

**Step 5: Keep simulator and host build trees separate**

Run:

```bash
rm -rf build_arm64_sim
```

Expected: a clean ARM64-only build directory is reserved for simulator runs.

### Task 3: Build and run the smallest ARM64 correctness subset first

**Files:**
- Modify: `none`
- Test: `memrpc/include/memrpc/core/runtime_utils.h`
- Test: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/src/server/rpc_server.cpp`

**Step 1: Configure a clean ARM64 build**

Run:

```bash
cmake -S . -B build_arm64_sim -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
```

Expected: configure succeeds on ARM64 without architecture-specific compile errors.

**Step 2: Build everything once**

Run:

```bash
cmake --build build_arm64_sim --parallel
```

Expected: all targets build successfully under ARM64 code generation.

**Step 3: Run focused memrpc tests first**

Run:

```bash
ctest --test-dir build_arm64_sim --output-on-failure -R "memrpc"
```

Expected: framework tests pass before involving the app layer.

**Step 4: Run focused virus_executor_service unit tests**

Run:

```bash
ctest --test-dir build_arm64_sim --output-on-failure -R "virus_executor_service_(session|heartbeat|codec|testkit_.*|health|policy|crash_recovery)"
```

Expected: unit-level service tests pass on ARM64.

**Step 5: Classify any failure immediately**

Expected:

```text
Compile failure -> architecture support gap
Deterministic test failure -> likely ARM64 correctness or memory-order bug
Timeout or flake -> likely scheduling/timing sensitivity, continue to repeat runs
```

### Task 4: Stress the ARM64 simulator for architecture-specific concurrency correctness

**Files:**
- Modify: `none`
- Test: `tools/build_and_test.sh`

**Step 1: Re-run the most concurrency-sensitive tests until failure**

Run:

```bash
tools/build_and_test.sh --timeout 90 --repeat until-fail:100 --test-regex "memrpc|virus_executor_service_(crash_recovery|testkit_async_pipeline|testkit_backpressure|testkit_dt_stability|testkit_stress_smoke)"
```

Expected: either all repeats pass or one of the targeted tests fails in a repeatable way.

**Step 2: Run the integration label on ARM64 simulator**

Run:

```bash
ctest --test-dir build_arm64_sim --output-on-failure -L integration
```

Expected: integration wiring works under ARM64 userland.

**Step 3: Run the stress label on ARM64 simulator**

Run:

```bash
ctest --test-dir build_arm64_sim --output-on-failure -L stress
```

Expected: no immediate correctness failures; timing may be slower than x86_64 and should not be judged as final performance evidence.

**Step 4: Run the DT label on ARM64 simulator**

Run:

```bash
ctest --test-dir build_arm64_sim --output-on-failure -L dt
```

Expected: deterministic recovery paths behave correctly on ARM64 codegen.

**Step 5: Save all failing logs separately from host results**

Run:

```bash
mkdir -p /tmp/arm64-sim-results
```

Expected: ARM64 simulator failures can be compared cleanly with host failures.

### Task 5: Add sanitizers in the ARM64 simulator only if the environment is stable

**Files:**
- Modify: `none`
- Test: `tools/build_and_test.sh`

**Step 1: Run ASan/UBSan on ARM64 simulator**

Run:

```bash
tools/build_and_test.sh --asan --test-regex "memrpc|virus_executor_service"
```

Expected: memory safety issues are surfaced with ARM64 code generation.

**Step 2: Run a reduced TSan subset if the simulator can tolerate it**

Run:

```bash
tools/build_and_test.sh --tsan --test-regex "memrpc|virus_executor_service_(testkit_async_pipeline|testkit_backpressure|crash_recovery)"
```

Expected: data-race findings, if any, are captured, but runtime may be much slower than native.

**Step 3: Stop using simulator timings as evidence**

Expected: sanitizer runtime and latency numbers on simulator are treated as diagnostic only, not product evidence.

**Step 4: Escalate every new failure to a minimal reproducer**

Expected: by the end of this task, every ARM64 simulator failure has one exact reproducer command.

**Step 5: Commit any test-only instrumentation added during debugging**

```bash
git add <only-test-or-debug-files>
git commit -m "test: add arm64 reproducer coverage"
```

### Task 6: Run the real ARM64 device validation matrix

**Files:**
- Modify: `none`
- Test: `tools/ci_sweep.sh`
- Test: `tools/push_gate.sh`

**Step 1: Build on the real ARM64 device or deploy the ARM64 build artifacts to it**

Run:

```bash
cmake -S . -B build_arm64_device -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
cmake --build build_arm64_device --parallel
```

Expected: native ARM64 device build succeeds.

**Step 2: Run the minimum real-device gate**

Run:

```bash
tools/push_gate.sh
```

Expected: the standard gate passes on native ARM64 timing.

**Step 3: Run the deep gate for lifecycle, queueing, recovery, or shutdown concerns**

Run:

```bash
tools/push_gate.sh --deep
```

Expected: concurrency-sensitive regressions are exercised under native scheduler and cache behavior.

**Step 4: Run the heavier practical sweep if the machine is suitable**

Run:

```bash
tools/ci_sweep.sh
```

Expected: strict, ASan, TSan subset, and repeated race checks run on native ARM64.

**Step 5: Add an overnight repeat for the highest-risk test if needed**

Run:

```bash
tools/build_and_test.sh --timeout 90 --repeat until-fail:500 --test-regex "memrpc_engine_death_handler_test|virus_executor_service_crash_recovery|virus_executor_service_testkit_stress_smoke"
```

Expected: long-tail flakiness either appears or confidence increases materially.

### Task 7: Decide whether a simulator failure or pass is actionable

**Files:**
- Modify: `none`
- Test: `docs/plans/2026-03-14-arm64-concurrency-validation-plan.md`

**Step 1: Treat simulator failures as actionable**

Expected:

```text
If it fails in ARM64 simulator and passes on x86_64, assume a real bug until disproved.
```

**Step 2: Do not treat simulator passes as sufficient evidence**

Expected:

```text
A simulator pass means "no obvious ARM64 correctness regression found", not "real ARM64 behavior is validated".
```

**Step 3: Treat native ARM64 stress and DT failures as higher priority than simulator-only timing noise**

Expected: triage prioritizes native failures first.

**Step 4: Use these exit criteria**

Expected:

```text
Required to claim ARM64 correctness:
- ARM64 simulator build passes
- ARM64 simulator focused repeat runs pass
- Native ARM64 push gate passes

Required to claim native ARM64 concurrency confidence:
- Native ARM64 deep gate passes
- At least one repeated stress or recovery path passes at scale
```

**Step 5: Write the final validation summary**

Include:

```text
Environment used
Exact commands run
Failing and passing tests
Whether result is simulator-only or native
Whether the issue is correctness, timing, or performance
```

### Task 8: Recommended minimum matrix for this repo

**Files:**
- Modify: `none`
- Test: `memrpc/include/memrpc/core/runtime_utils.h`
- Test: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/src/server/rpc_server.cpp`

**Step 1: Run this minimum set on ARM64 simulator**

Run:

```bash
cmake -S . -B build_arm64_sim -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
cmake --build build_arm64_sim --parallel
ctest --test-dir build_arm64_sim --output-on-failure -R "memrpc"
ctest --test-dir build_arm64_sim --output-on-failure -R "virus_executor_service_(session|heartbeat|codec|testkit_.*|health|policy|crash_recovery)"
tools/build_and_test.sh --timeout 90 --repeat until-fail:100 --test-regex "virus_executor_service_(testkit_async_pipeline|testkit_backpressure|crash_recovery|testkit_stress_smoke)"
```

Expected: ARM64 codegen and the main concurrency-sensitive correctness paths are covered.

**Step 2: Run this minimum set on native ARM64**

Run:

```bash
tools/push_gate.sh
tools/push_gate.sh --deep
```

Expected: the repo’s standard and concurrency-heavy gates pass on native ARM64.

**Step 3: Only add perf comparison after correctness is green**

Run:

```bash
ctest --test-dir build_arm64_device --output-on-failure -R "virus_executor_service_testkit_(latency|throughput)"
```

Expected: performance data is gathered only after correctness and stability are established.

**Step 4: Stop if the simulator already exposes a deterministic ARM64-only failure**

Expected: fix that bug before spending time on broader native sweeps.

**Step 5: Escalate to native immediately if the concern is `CpuRelax()` effectiveness**

Expected: simulator can validate code path selection, but native hardware is required to judge spin behavior, fairness, and contention impact.
