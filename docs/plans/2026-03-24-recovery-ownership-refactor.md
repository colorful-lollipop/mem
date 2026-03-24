# Recovery Ownership Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move generic recovery state observation/waiting into `memrpc::RpcClient` and simplify `VesClient` into a thin VES-specific adapter.

**Architecture:** `RpcClient` already owns the recovery state machine, lifecycle transitions, and health/death triggers. This refactor extends that ownership to recovery-state observation so `VesClient` no longer mirrors snapshots or runs its own condition-variable loop; it only maps VES policy and uses framework recovery primitives for shared-memory and AnyCall paths.

**Tech Stack:** C++17, CMake/Ninja, GoogleTest, memrpc framework, virus_executor_service client/transport layers

---

### Task 1: Add framework recovery observation support

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/`

**Step 1: Add a monotonic recovery observation token to the runtime snapshot**

Add a framework-owned version field that changes whenever recovery-observable state changes.

**Step 2: Add a public wait helper on `RpcClient`**

Expose a method that waits until the recovery observation token changes or a deadline expires.

**Step 3: Wire version bumps into lifecycle transitions**

Increment the observation token on lifecycle transitions and session-ready transitions, then notify the existing recovery condition variable.

### Task 2: Remove mirrored recovery state from `VesClient`

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`

**Step 1: Delete cached recovery state fields and helper methods**

Remove `recoverySnapshot_`, `recoveryCv_`, `recoveryStateVersion_`, `CacheRecoverySnapshot`, `CacheRecoveryEvent`, and the local wait helper.

**Step 2: Use framework snapshot directly**

Make `GetRecoveryRuntimeSnapshot()` and `EngineDied()` read from `client_.GetRecoveryRuntimeSnapshot()`.

**Step 3: Keep only VES-specific retry orchestration**

Retain the outer retry wrapper only for the `AnyCall` fallback path, using the new `RpcClient` wait helper instead of local bookkeeping.

### Task 3: Keep callback responsibilities narrow

**Files:**
- Modify: `virus_executor_service/src/client/ves_client.cpp`

**Step 1: Stop using policy callbacks to refresh mirrored state**

Remove the `notifyRecoveryState` callback plumbing from `BuildRecoveryPolicy`.

**Step 2: Keep only policy decisions and VES logging**

`BuildRecoveryPolicy` should just decide restart/idle-close and emit VES-specific diagnostics.

### Task 4: Verify targeted recovery behavior

**Files:**
- Test: `memrpc/tests/*recovery*`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Build the affected targets**

Run a targeted build for memrpc and VES recovery-related tests.

**Step 2: Run focused recovery tests**

Verify external-health recovery, engine-death recovery, and AnyCall cooldown retry behavior.
