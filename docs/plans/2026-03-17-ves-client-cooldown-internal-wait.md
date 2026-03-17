# VesClient Cooldown Internal Wait Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move cooldown/recovery waiting inside the client stack so VesClient APIs do not need per-method retry loops.

**Architecture:** Keep explicit `CooldownActive` behavior available in memrpc by default, but add an opt-in per-call recovery wait mode. VesClient uses that mode for memrpc requests and a single shared retry helper for direct `AnyCall` fallback, so future APIs stay one-line wrappers instead of duplicating cooldown loops.

**Tech Stack:** C++17, memrpc RpcClient, VesClient, GoogleTest

---

### Task 1: Add opt-in recovery wait to memrpc admission

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`

**Step 1: Add per-call recovery wait fields**

Extend `MemRpc::RpcCall` with:
- `bool waitForRecovery = false`
- `uint32_t recoveryTimeoutMs = 0`

Document that this only affects cooldown/restart recovery, not queue admission.

**Step 2: Implement recovery-aware submit path**

In `RpcClient::Impl`, when `EnsureLiveSession()` returns `CooldownActive` and `waitForRecovery` is enabled:
- wait inside the submit loop instead of failing immediately
- stop waiting when cooldown/session state changes, timeout expires, or client shuts down
- preserve old behavior for calls without the flag

Also allow the same recovery budget to cover post-cooldown reconnect retries that still return `PeerDisconnected`.

**Step 3: Add focused framework test**

Add a test that:
- triggers external recovery with a cooldown delay
- submits a call with `waitForRecovery = true`
- starts/reuses the server after the delay window
- verifies the call blocks internally and returns `Ok` without surfacing `CooldownActive`

### Task 2: Collapse VesClient retry logic into one internal helper

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Add shared recovery wait helper state**

Add a condition-variable based helper in `VesClient` that:
- computes one recovery wait budget from configured restart delays plus grace
- waits for either recovery-related notifications or a short retry poll interval
- can be reused by every API method

**Step 2: Wire recovery notifications**

Notify waiters from:
- `RequestRecovery`
- exec-timeout restart policy
- engine-death restart policy
- session-ready callback after recovery completes

**Step 3: Replace ScanFile special loop**

Refactor `ScanFile()` so it:
- performs one attempt through a common invoke lambda
- uses memrpc internal recovery wait for shared-memory requests
- uses the shared VesClient retry helper for `AnyCall` fallback retries

**Step 4: Add or update VesClient regression tests**

Verify that:
- `ScanFile()` still succeeds across restart cooldown
- no per-call cooldown loop remains in the public API path
- large-payload `AnyCall` fallback also survives the same recovery window
