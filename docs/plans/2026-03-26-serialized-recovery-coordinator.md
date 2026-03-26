# Serialized Recovery Coordinator Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Funnel recovery-producing signals through one serialized coordinator so stale session events cannot close or recover a newer session.

**Architecture:** Add an internal recovery worker thread that drains a typed event queue, validates each event against an internal session snapshot generation, and performs recovery actions while holding a lifecycle coordination mutex shared with demand-open and shutdown paths. Keep the public `RpcClient` API stable; changes stay inside `memrpc/src/client/rpc_client.cpp` plus focused tests.

**Tech Stack:** C++17, std::thread, std::mutex, std::condition_variable, GoogleTest, CMake/Ninja

---

### Task 1: Add Internal Session Generations

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/engine_death_handler_test.cpp`

**Step 1: Extend the internal `SessionSnapshot` and `PendingInfo` structs**

- Add `generation` to `SessionSnapshot`.
- Add `sessionGeneration` to `PendingInfo`.
- Keep the public API unchanged.

**Step 2: Publish generations from `ClientSessionTransport`**

- Add a monotonically increasing `nextSnapshotGeneration_` guarded by `sessionMutex_`.
- Stamp every published snapshot, including empty/no-session snapshots.
- Carry generation through duplicated wait-handle metadata.

**Step 3: Update internal readers**

- Make `MakePendingInfo`, wait-fd tracking, and recovery entrypoints consume the captured generation instead of re-reading only `sessionId`.

**Step 4: Build-check**

Run: `cmake --build build_ninja --parallel`
Expected: success

### Task 2: Introduce Serialized Recovery Event Processing

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Test: `memrpc/tests/engine_death_handler_test.cpp`

**Step 1: Add an internal recovery event queue and worker**

- Add a `RecoveryCoordinator` helper with:
  - event enum (`EngineDeath`, `ExternalRequest`, `FailureDecision`, `IdleDecision`)
  - per-event observed session snapshot
  - queue mutex/CV
  - worker loop

**Step 2: Add lifecycle serialization**

- Add an `lifecycleMutex_` in `RpcClient::Impl`.
- Hold it while:
  - opening a session in `EnsureLiveSession`
  - executing queued recovery events
  - performing shutdown session teardown
  - handling direct transport-triggered forced closes

**Step 3: Route recovery sources through the coordinator**

- Replace direct recovery execution in:
  - engine-death handling
  - external recovery requests
  - failure-policy recovery decisions
  - idle-policy recovery decisions
- Keep source threads lightweight: capture context and enqueue only.

**Step 4: Drop stale events by generation**

- Before acting on an event, compare the queued observed generation with the current snapshot generation.
- Ignore stale events and log them.
- Preserve current behavior for duplicate engine-death signals by making the second event stale after the first closes the session.

### Task 3: Keep Existing Recovery Semantics While Centralizing Actions

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_recovery_policy_test.cpp`

**Step 1: Move action execution behind coordinator-owned helpers**

- Add helpers that run under `lifecycleMutex_` for:
  - restart recovery
  - idle close
  - engine-death close + pending resolution + policy callback
  - no-session transitions

**Step 2: Preserve current pending-request handling**

- Keep:
  - `ResolvePeerDisconnected` for external recovery
  - `FailWithRecoveryPolicy` for failure/idle recovery
  - `KeepCurrentState` for engine-death recovery policy

**Step 3: Keep demand reconnect behavior**

- Preserve immediate reconnect when delay is zero.
- Preserve cooldown behavior when delay is non-zero.

### Task 4: Add Focused Regression Coverage

**Files:**
- Modify: `memrpc/tests/engine_death_handler_test.cpp`
- Modify: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Modify: `memrpc/tests/rpc_client_shutdown_race_test.cpp` if needed

**Step 1: Cover stale engine-death events after session replacement**

- Assert a delayed old-session death signal does not tear down the new session.

**Step 2: Cover duplicate recovery requests**

- Assert duplicate engine-death or external recovery events for the same observed generation do not double-apply policy or close counts.

**Step 3: Cover queued recovery across shutdown**

- Assert shutdown remains bounded and queued recovery does not resurrect or mutate closed state.

### Task 5: Verify

**Files:**
- No code changes

**Step 1: Build**

Run: `cmake --build build_ninja --parallel`
Expected: success

**Step 2: Run focused tests**

Run: `ctest --test-dir build_ninja --output-on-failure -R "memrpc_(engine_death_handler|rpc_client_external_recovery|rpc_client_shutdown_race)_test"`
Expected: all selected tests pass

**Step 3: If shared-memory sandboxing blocks tests, rerun with escalation**

Run: `ctest --test-dir build_ninja --output-on-failure -R "memrpc_(engine_death_handler|rpc_client_external_recovery|rpc_client_shutdown_race)_test"`
Expected: all selected tests pass outside sandbox
