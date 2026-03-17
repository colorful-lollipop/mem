# DFX Recovery State Machine Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the current ad-hoc mix of manual shutdown, crash recovery, timeout restart, health-triggered recovery, and idle close with one explicit client lifecycle state machine and a single DFX/reporting surface.

**Architecture:** Keep detection, policy, execution, and observation separate. `memrpc::RpcClient` becomes the single owner of lifecycle state transitions and recovery execution; `VesClient` only configures policy and consumes structured state/DFX reports. Idle management stays framework-owned, explicit `Shutdown()` becomes terminal and never feeds recovery again, and `AnyCall` remains a pure VES-side transport downgrade path rather than part of the memrpc lifecycle model.

**Tech Stack:** C++17, memrpc `RpcClient`, `VesClient`, GoogleTest, CMake/CTest

---

### Task 1: Define the unified lifecycle model

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`
- Test: `memrpc/tests/rpc_client_recovery_policy_test.cpp`

**Step 1: Write the failing API-shape test**

Add a test in `memrpc/tests/rpc_client_recovery_policy_test.cpp` that asserts the new lifecycle/DFX structs compile and expose:
- a client lifecycle enum
- a recovery trigger enum
- a terminal/manual shutdown distinction
- a structured runtime snapshot/report type

Expected first failure: missing type/field names.

**Step 2: Add explicit lifecycle and trigger enums**

Add to `memrpc/include/memrpc/client/rpc_client.h`:
- `enum class ClientLifecycleState`
- `enum class RecoveryTrigger`
- `struct RecoveryRuntimeSnapshot`
- `struct RecoveryEventReport`

Required states:
- `Uninitialized`
- `Active`
- `Cooldown`
- `IdleClosed`
- `Recovering`
- `Closed`

Required triggers:
- `ManualShutdown`
- `ExecTimeout`
- `EngineDeath`
- `ExternalHealthSignal`
- `IdlePolicy`
- `DemandReconnect`

**Step 3: Document the state machine**

Update:
- `docs/architecture.md`
- `docs/sa_integration.md`

Document:
- which transitions are legal
- that `Shutdown()` is terminal
- that a client instance cannot be re-`Init()`ed after `Shutdown()`
- that idle close is framework-managed and non-terminal
- that `Cooldown` and `Recovering` are internal framework states

**Step 4: Run the targeted compile-level tests**

Run: `tools/build_and_test.sh --build-only --test-regex 'memrpc_rpc_client_recovery_policy_test'`
Expected: PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h docs/architecture.md docs/sa_integration.md memrpc/tests/rpc_client_recovery_policy_test.cpp
git commit -m "feat: define unified recovery lifecycle model"
```

### Task 2: Move lifecycle ownership into RpcClient

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Test: `memrpc/tests/engine_death_handler_test.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Test: `memrpc/tests/rpc_client_idle_callback_test.cpp`

**Step 1: Write failing lifecycle transition tests**

Add tests covering:
- `Shutdown()` keeps client terminal and blocks any later recovery trigger
- `Shutdown()` keeps the instance permanently terminal and `Init()` after `Shutdown()` returns `ClientClosed`
- idle close transitions to `IdleClosed` and reopens only on demand
- engine death / timeout / health signal enter recovery states but not after manual close

Expected first failure: lifecycle snapshot/report fields unavailable or wrong.

**Step 2: Introduce a single transition helper**

In `memrpc/src/client/rpc_client.cpp`, replace scattered direct state edits with one helper:
- `TransitionLifecycle(...)`
- `ScheduleRecovery(...)`
- `EnterIdleClosed(...)`
- `EnterTerminalClosed(...)`

Required cleanup:
- `BeginRestart`
- `HandleEngineDeath`
- `RequestExternalRecovery`
- `Shutdown`
- idle policy path

The helper must own:
- `recoveryPending_`
- `cooldownUntilMs_`
- session close/open reason
- DFX event emission

**Step 3: Split non-terminal idle close from terminal close**

Keep `RecoveryAction::CloseSession` semantics as:
- close live session
- keep client reusable
- mark lifecycle `IdleClosed`
- next user request may reconnect

Do not let this path set `ClientClosed`.

**Step 4: Make `Shutdown()` the only terminal public close**

`Shutdown()` must:
- set lifecycle `Closed`
- clear pending recovery
- clear cooldown
- ignore later engine death / external recovery / health watchdog signals
- make all future calls return `ClientClosed`
- make later `Init()` on the same object return `ClientClosed`

**Step 5: Run focused lifecycle tests**

Run: `ctest --test-dir build_ninja --output-on-failure -R 'memrpc_engine_death_handler_test|memrpc_rpc_client_external_recovery_test|memrpc_rpc_client_idle_callback_test'`
Expected: PASS

**Step 6: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/include/memrpc/client/rpc_client.h memrpc/tests/engine_death_handler_test.cpp memrpc/tests/rpc_client_external_recovery_test.cpp memrpc/tests/rpc_client_idle_callback_test.cpp
git commit -m "feat: centralize rpc client lifecycle transitions"
```

### Task 3: Collapse DFX reporting into one structured surface

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`

**Step 1: Write failing DFX tests**

Add tests asserting:
- VES can read a unified last-recovery reason/state
- `EngineDied()` is only a test/DFX helper view derived from structured reports, not a sticky business semantic
- idle close and manual shutdown produce distinct reasons

Expected first failure: missing callbacks/report plumbing.

**Step 2: Add runtime snapshot/report callback**

Add to `RpcClient`:
- `SetRecoveryEventCallback(...)`
- `GetRecoveryRuntimeSnapshot()`

Include:
- lifecycle state
- last trigger
- last recovery action
- cooldown remaining
- last session ids
- terminal/manual-close flag

`RecoveryEventReport` should represent the latest transition/reopen reason only. It does **not** need to preserve a sticky historical fault cause across later transitions such as `DemandReconnect`.

**Step 3: Make VesClient consume structured reports**

Refactor `virus_executor_service/src/client/ves_client.cpp` so it no longer tracks ad-hoc booleans beyond cached view state. Replace:
- `engineDied_`
- recovery wake heuristics

with a small cached projection of `RpcClient` runtime snapshot/report.

Keep `EngineDied()` only as a compatibility/testing helper derived from the cached structured view. It should not drive policy and does not need “last fault forever” semantics.

**Step 4: Run VES DFX tests**

Run: `tools/build_and_test.sh --build-only --test-regex 'ves_recovery_reason_test|ves_policy_test'`
Expected: PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h memrpc/src/client/rpc_client.cpp virus_executor_service/include/client/ves_client.h virus_executor_service/src/client/ves_client.cpp virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp
git commit -m "feat: unify recovery dfx reporting"
```

### Task 4: Simplify VesClient policy wiring

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp`

**Step 1: Write failing policy composition tests**

Cover:
- timeout restart stays enabled
- engine death restart stays enabled
- idle close remains framework-managed
- `Shutdown()` stays terminal
- no public VES API has a handwritten cooldown loop
- `AnyCall` fallback stays a VES-owned downgrade path and does not mutate or reinterpret `RpcClient` lifecycle state

Expected first failure: old bespoke retry/helper assumptions.

**Step 2: Extract one policy builder**

Create a local helper in `virus_executor_service/src/client/ves_client.cpp`:
- `BuildRecoveryPolicy(const VesClientOptions&)`

Move all policy lambdas into it.
Keep `VesClient` itself focused on:
- bootstrap wiring
- request invocation
- DFX callback bridging

**Step 3: Reduce per-method recovery code**

Preserve the current internal wait helper, but make it consume the new `RpcClient` lifecycle snapshot/report instead of mixing:
- cooldown check
- recoveryPending check
- local wake epochs

Each future VES method should become:
- encode request
- invoke common helper
- decode reply

For large requests, keep `AnyCall` as a simple transport downgrade:
- no memrpc lifecycle transitions are synthesized from `AnyCall` success
- no requirement that `AnyCall` success implies memrpc lifecycle has already returned to `Active`

**Step 4: Run focused VES tests**

Run: `ctest --test-dir build_ninja --output-on-failure -R 'virus_executor_service_policy_test|virus_executor_service_health_subscription_test'`
Expected: PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/client/ves_client.h virus_executor_service/src/client/ves_client.cpp virus_executor_service/tests/unit/ves/ves_policy_test.cpp virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp
git commit -m "refactor: simplify ves client recovery policy wiring"
```

### Task 5: Align testkit DFX tooling with the unified model

**Files:**
- Modify: `virus_executor_service/tests/unit/testkit/testkit_resilient_invoker.h`
- Modify: `virus_executor_service/tests/unit/testkit/testkit_resilient_invoker.cpp`
- Modify: `virus_executor_service/tests/unit/testkit/testkit_dfx_test.cpp`

**Step 1: Write failing replay/DFX tests**

Add assertions that testkit replay decisions consume the unified recovery reports instead of inferring from scattered failure statuses alone.

Expected first failure: missing report accessors.

**Step 2: Refactor resilient invoker to use recovery reports**

Update `ResilientBatchInvoker` to classify failures using:
- lifecycle state
- trigger
- replay-safe hint
- terminal/manual-close distinction

Stop open-coding separate “engine died / timeout / disconnected” heuristics.

**Step 3: Run testkit DFX tests**

Run: `tools/build_and_test.sh --build-only --test-regex 'testkit_dfx_test'`
Then: `ctest --test-dir build_ninja --output-on-failure -R 'virus_executor_service_testkit_dfx_test'`
Expected: PASS

**Step 4: Commit**

```bash
git add virus_executor_service/tests/unit/testkit/testkit_resilient_invoker.h virus_executor_service/tests/unit/testkit/testkit_resilient_invoker.cpp virus_executor_service/tests/unit/testkit/testkit_dfx_test.cpp
git commit -m "refactor: align testkit dfx replay with unified recovery model"
```

### Task 6: Final verification sweep

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`

**Step 1: Run the focused verification matrix**

Run:
- `ctest --test-dir build_ninja --output-on-failure -R 'memrpc_engine_death_handler_test|memrpc_rpc_client_external_recovery_test|memrpc_rpc_client_idle_callback_test'`
- `ctest --test-dir build_ninja --output-on-failure -R 'virus_executor_service_policy_test|virus_executor_service_health_subscription_test|virus_executor_service_recovery_reason_test|virus_executor_service_testkit_dfx_test'`

Expected: PASS

Compatibility note:
- recovery validation should prefer “new client object after terminal shutdown” semantics
- do not require same-object reuse after `Shutdown()`

**Step 2: Run the push gate if the refactor is merged in one branch**

Run: `tools/push_gate.sh`
Expected: PASS

**Step 3: Commit docs touch-ups**

```bash
git add docs/architecture.md docs/sa_integration.md
git commit -m "docs: describe unified recovery lifecycle and dfx model"
```
