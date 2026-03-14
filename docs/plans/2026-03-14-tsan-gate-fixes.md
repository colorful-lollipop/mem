# TSan Gate Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate the TSan races and timeout that currently fail the deep push gate.

**Architecture:** Fix the shared root causes instead of patching individual tests. The plan hardens `memrpc` bootstrap/session recovery against concurrent teardown, removes mock system-ability callbacks that outlive their stub objects, and trims the async throughput test budget only when ThreadSanitizer is enabled.

**Tech Stack:** C++17, GoogleTest, CMake, ThreadSanitizer

---

### Task 1: Serialize `DevBootstrapChannel` mutable state

**Files:**
- Modify: `memrpc/src/bootstrap/dev_bootstrap.cpp`
- Test: `memrpc/tests/engine_death_handler_test.cpp`

**Step 1: Reproduce the race**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex memrpc_engine_death_handler_test`

Expected: TSan reports a race in `DevBootstrapChannel::Impl::EnsureInitialized()`.

**Step 2: Add a mutex around mutable bootstrap state**

Guard access to `handles`, `initialized`, and `death_callback`, and invoke the death callback after releasing the lock.

**Step 3: Re-run the focused test**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex memrpc_engine_death_handler_test`

Expected: test passes without a TSan report.

### Task 2: Remove submitter-vs-session teardown races

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/src/core/session.cpp` if helper changes are required
- Test: `virus_executor_service/tests/unit/testkit/testkit_client_test.cpp`
- Test: `virus_executor_service/tests/unit/testkit/testkit_dfx_test.cpp`

**Step 1: Reproduce the recovery-path races**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex 'virus_executor_service_testkit_(client|dfx)_test'`

Expected: TSan reports a `close()` race while the client recovery path tears down a session.

**Step 2: Make request-credit waits stable across session teardown**

Duplicate the request-credit fd before polling, wake the old credit fd before resetting a session, and keep teardown/recovery logic consistent with replay.

**Step 3: Re-run the focused tests**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex 'virus_executor_service_testkit_(client|dfx)_test'`

Expected: both tests pass without TSan reports.

### Task 3: Fix mock SA lifetime races

**Files:**
- Modify: `third_party/ohos_sa_mock/include/iremote_stub.h`
- Modify: `third_party/ohos_sa_mock/include/mock_service_socket.h`
- Modify: `third_party/ohos_sa_mock/src/mock_service_socket.cpp`
- Modify: `third_party/ohos_sa_mock/src/system_ability_manager.cpp` if transport startup/shutdown needs adjustment
- Test: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Reproduce the SA transport races**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex 'virus_executor_service_(heartbeat|health_subscription|policy)_test'`

Expected: TSan reports races in `IRemoteStub` destruction and `MockServiceSocket::Stop()`.

**Step 2: Remove stale callbacks and synchronize socket shutdown**

Make `IRemoteObject` callbacks resolve the broker through a weak pointer instead of a raw stub pointer, and make listen-fd shutdown atomic with respect to the accept loop.

**Step 3: Re-run the focused tests**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex 'virus_executor_service_(heartbeat|health_subscription|policy)_test'`

Expected: all three tests pass without TSan reports.

### Task 4: Keep the async throughput test inside the TSan budget

**Files:**
- Modify: `virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp`
- Test: `virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp`

**Step 1: Reproduce the timeout**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex virus_executor_service_testkit_async_pipeline_test`

Expected: the test times out near the CTest limit.

**Step 2: Reduce the default perf budget only under TSan**

Keep the same behavior and assertions, but shorten default warmup/runtime under ThreadSanitizer while preserving env-var overrides.

**Step 3: Re-run the focused test**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex virus_executor_service_testkit_async_pipeline_test`

Expected: the test completes under the timeout.

### Task 5: Verify the repaired gate subset

**Files:**
- Modify: none
- Test: existing gate scripts

**Step 1: Run the previously failing TSan subset**

Run: `tools/build_and_test.sh --build-dir build_tsan --tsan --strict --werror --test-regex 'memrpc_engine_death_handler_test|virus_executor_service_(heartbeat|health_subscription|policy|testkit_client_test|testkit_dfx_test|testkit_async_pipeline_test)'`

Expected: all targeted tests pass.

**Step 2: Run the full deep gate if the subset is clean**

Run: `tools/push_gate.sh --deep`

Expected: full gate passes.
