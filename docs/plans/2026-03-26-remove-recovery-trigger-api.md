# Remove RecoveryTrigger API Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove `RecoveryTrigger` from the public `RpcClient` recovery API while preserving recovery behavior and simplifying the internal recovery state machine.

**Architecture:** Collapse recovery observability onto lifecycle state, cooldown, and session ids only. Remove trigger-carrying state from `ClientRecoveryState`, update downstream callers/tests to assert behavior and lifecycle transitions instead of trigger provenance, and keep recovery execution paths unchanged.

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: Shrink the public recovery API

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Test: `memrpc/tests/rpc_client_recovery_policy_test.cpp`

**Step 1: Write the failing test**

Remove assertions that depend on `RecoveryTrigger`, and keep default-value coverage for the remaining snapshot/report fields.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build_ninja --output-on-failure -R rpc_client_recovery_policy_test`
Expected: compile/test failure until the header and implementation are updated.

**Step 3: Write minimal implementation**

Delete the public `RecoveryTrigger` enum and remove `lastTrigger` / `trigger` from `RecoveryRuntimeSnapshot` and `RecoveryEventReport`.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build_ninja --output-on-failure -R rpc_client_recovery_policy_test`
Expected: PASS

### Task 2: Simplify the internal recovery state machine

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/engine_death_handler_test.cpp`
- Test: `memrpc/tests/rpc_client_idle_callback_test.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Test: `memrpc/tests/rpc_client_shutdown_race_test.cpp`
- Test: `memrpc/tests/rpc_client_api_test.cpp`

**Step 1: Write the failing test**

Replace trigger-based expectations with state-sequence expectations:
- idle close transitions to `IdleClosed`
- external recovery / engine death transitions through `Cooldown` or `Recovering`
- shutdown transitions to `Closed`
- shutdown races preserve lifecycle invariants rather than trigger identity

**Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build_ninja --output-on-failure -R "engine_death_handler_test|rpc_client_idle_callback_test|rpc_client_external_recovery_test|rpc_client_shutdown_race_test|rpc_client_api_test"`
Expected: compile/test failure until the internal trigger machinery is removed.

**Step 3: Write minimal implementation**

In `ClientRecoveryState`:
- remove `nextSessionOpenTrigger_`
- remove `lastRecoveryTrigger_`
- remove `PendingSessionOpenTriggerLocked()`
- remove trigger parameters from lifecycle transition helpers
- collapse `EnterDemandReconnect()` into the generic recovering transition

In `RpcClient::Impl`:
- remove trigger arguments from recovery scheduling / no-session transitions
- keep recovery actions and pending-request handling unchanged

**Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build_ninja --output-on-failure -R "engine_death_handler_test|rpc_client_idle_callback_test|rpc_client_external_recovery_test|rpc_client_shutdown_race_test|rpc_client_api_test"`
Expected: PASS

### Task 3: Update downstream diagnostics and app-level tests

**Files:**
- Modify: `virus_executor_service/src/app/ves_client_main.cpp`
- Modify: `virus_executor_service/tests/unit/testkit/testkit_resilient_invoker.cpp`
- Modify: `virus_executor_service/tests/unit/testkit/testkit_dfx_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Modify: `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`

**Step 1: Write the failing test**

Replace trigger-based assertions/filters with lifecycle-state or recovery-pending assertions, and remove trigger logging from app diagnostics.

**Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build_ninja --output-on-failure -R "virus_executor_service_testkit_dfx_test|ves_policy_test|ves_crash_recovery_test"`
Expected: compile/test failure until the downstream code is updated.

**Step 3: Write minimal implementation**

Adjust downstream code to consume the smaller recovery snapshot/report surface:
- use `state`, `recoveryPending`, and session ids instead of trigger provenance
- keep event capture and runtime snapshots available for diagnostics

**Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build_ninja --output-on-failure -R "virus_executor_service_testkit_dfx_test|ves_policy_test|ves_crash_recovery_test"`
Expected: PASS

### Task 4: Verify the end-to-end impacted surface

**Files:**
- Modify: none
- Test: `tools/build_and_test.sh`

**Step 1: Run focused build/test verification**

Run: `tools/build_and_test.sh --build-only`
Expected: build succeeds

**Step 2: Run focused recovery/unit verification**

Run: `ctest --test-dir build_ninja --output-on-failure -R "rpc_client_(recovery_policy|idle_callback|external_recovery|shutdown_race|api)_test|engine_death_handler_test|virus_executor_service_testkit_dfx_test|ves_policy_test"`
Expected: PASS

**Step 3: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h \
        memrpc/src/client/rpc_client.cpp \
        memrpc/tests \
        virus_executor_service/src/app/ves_client_main.cpp \
        virus_executor_service/tests
git commit -m "refactor: remove recovery trigger api"
```
