# RpcClient State Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace `RpcClient::Impl`'s scattered run/close atomics with explicit internal state objects so shutdown, init, and submit gating become easier to read without collapsing recovery into a second giant state machine.

**Architecture:** Keep `rpc_client.cpp` as a single implementation file. Introduce two small state domains inside `RpcClient::Impl`: `ApiLifecycleState` for public API availability and `WorkerRunState` for background-thread lifetime. Leave `RecoveryCoordinator` as the only owner of recovery lifecycle; the new states only gate API entry, thread loops, and shutdown sequencing.

**Tech Stack:** C++17, std::atomic, GoogleTest, CMake/CTest, existing `memrpc` client internals

---

### Task 1: Lock in current terminal semantics with focused tests

**Files:**
- Modify: `memrpc/tests/rpc_client_api_test.cpp`
- Modify: `memrpc/tests/rpc_client_shutdown_race_test.cpp`

**Step 1: Write the failing test**

Add an API test that asserts:

```cpp
TEST(RpcClientApiTest, ShutdownPreventsLaterInvokeAsyncFromQueueingWork) {
  MemRpc::RpcClient client(std::make_shared<...>());
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);
  client.Shutdown();

  MemRpc::RpcReply reply;
  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::ClientClosed);
  EXPECT_EQ(reply.status, MemRpc::StatusCode::ClientClosed);
}
```

Add a shutdown-race test that repeatedly interleaves `InvokeAsync()` and `Shutdown()` and only accepts `ClientClosed` or already-completed in-flight outcomes, never a hang.

**Step 2: Run test to verify it fails**

Run: `tools/build_and_test.sh --test-regex 'rpc_client_api_test|rpc_client_shutdown_race_test'`

Expected: at least one new assertion fails before the refactor starts.

**Step 3: Keep test scope narrow**

Do not add recovery assertions here. These tests should only lock down:
- terminal shutdown is permanent
- no post-shutdown enqueue semantics leak through public API
- shutdown remains fast under contention

**Step 4: Run test to verify the focused baseline**

Run: `tools/build_and_test.sh --test-regex 'rpc_client_api_test|rpc_client_shutdown_race_test'`

Expected: the new failures are stable and reproducible.

**Step 5: Commit**

```bash
git add memrpc/tests/rpc_client_api_test.cpp memrpc/tests/rpc_client_shutdown_race_test.cpp
git commit -m "test: lock rpc client terminal shutdown semantics"
```

### Task 2: Introduce explicit API lifecycle state

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_api_test.cpp`

**Step 1: Add the new state type near other file-local helpers**

Add:

```cpp
enum class ApiLifecycleState : uint8_t {
  Open,
  Closing,
  Closed,
};
```

Add helper methods on `RpcClient::Impl`:

```cpp
ApiLifecycleState LoadApiState() const;
bool IsApiOpen() const;
bool IsApiTerminal() const;
bool BeginShutdown();
void FinishShutdown();
StatusCode ApiRejectionStatus() const;
```

**Step 2: Run build to verify the scaffolding compiles**

Run: `tools/build_and_test.sh --test-regex rpc_client_api_test`

Expected: compile may fail until call sites are switched; iterate until the file builds.

**Step 3: Replace `clientClosed_` reads in public-entry and lifecycle gates**

Convert these paths to the new helpers:
- `Init()`
- `InvokeAsync()`
- `EnsureLiveSession()`
- `RequestExternalRecovery()`
- `HandleEngineDeath()`
- `MaybeRunHealthCheck()`
- `EnterIdleClosed()`
- `EnterDisconnected()`
- `ScheduleRecovery()`

Target shape:

```cpp
if (IsApiTerminal()) {
  return StatusCode::ClientClosed;
}
```

and:

```cpp
if (!BeginShutdown()) {
  return;
}
```

**Step 4: Preserve the existing semantic split**

Do not merge `ApiLifecycleState` into `ClientLifecycleState`. Recovery remains owned by `RecoveryCoordinator`; API terminal state only decides whether new work may enter and whether callbacks/signals are ignored.

**Step 5: Run tests**

Run: `tools/build_and_test.sh --test-regex 'rpc_client_api_test|rpc_client_external_recovery_test'`

Expected: API tests pass and external recovery still disables itself after manual shutdown.

**Step 6: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_api_test.cpp
git commit -m "refactor: add rpc client api lifecycle state"
```

### Task 3: Move shutdown sequencing onto API lifecycle helpers

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_shutdown_race_test.cpp`

**Step 1: Refactor `Shutdown()` into explicit phases**

Reshape `Shutdown()` around:

```cpp
if (!BeginShutdown()) {
  return;
}
EnterTerminalClosed();
ClearCallbacks();
StopThreads();
FailOutstandingWork();
FinishShutdown();
```

Add small helpers:

```cpp
void ClearCallbacks();
void FailQueuedSubmissions(StatusCode status);
void FailOutstandingWork(StatusCode status);
```

**Step 2: Ensure phase names match behavior**

- `BeginShutdown()` transitions `Open -> Closing`
- `EnterTerminalClosed()` closes the live session and updates recovery snapshot
- `FinishShutdown()` transitions `Closing -> Closed`

Do not mark `Closed` before worker joins and queue draining are complete.

**Step 3: Re-run the race test**

Run: `tools/build_and_test.sh --test-regex rpc_client_shutdown_race_test`

Expected: fast shutdown tests still pass; no deadlock or repeated-close regression appears.

**Step 4: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_shutdown_race_test.cpp
git commit -m "refactor: make rpc client shutdown phases explicit"
```

### Task 4: Introduce explicit worker run state

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_shutdown_race_test.cpp`

**Step 1: Add the worker state type**

Add:

```cpp
enum class WorkerRunState : uint8_t {
  NotStarted,
  Running,
  Stopping,
  Stopped,
};
```

Add helpers:

```cpp
WorkerRunState LoadWorkerState() const;
bool WorkersRunning() const;
bool BeginWorkerStart();
bool BeginWorkerStop();
void FinishWorkerStop();
```

**Step 2: Move `StartThreads()` and `StopThreads()` onto the new state**

Desired behavior:
- `StartThreads()` only succeeds from `NotStarted` or `Stopped`
- `StopThreads()` only performs wake/join once from `Running`
- loops and waits use `WorkersRunning()` instead of directly reading an atomic bool

**Step 3: Keep this state separate from API lifecycle**

The system may legally be:
- `ApiLifecycleState::Closing` while `WorkerRunState::Running`
- `ApiLifecycleState::Closed` only after `WorkerRunState::Stopped`

That distinction is the main readability win; do not collapse them back together.

**Step 4: Run the shutdown and watchdog suite**

Run: `tools/build_and_test.sh --test-regex 'rpc_client_shutdown_race_test|rpc_client_timeout_watchdog_test'`

Expected: waits and wakeups still terminate promptly.

**Step 5: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_shutdown_race_test.cpp
git commit -m "refactor: introduce rpc client worker run state"
```

### Task 5: Switch worker loops and waits to the new run-state API

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_eventfd_fault_injection_test.cpp`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Replace `running_` checks in worker loops**

Update:
- `SubmitWorker::Run()`
- `SubmitWorker::SubmitOne()`
- `ResponseWorker::Run()`
- `WatchdogLoop()`
- `WaitForRequestCredit()`
- `WaitForRecovery()`
- `WaitForRecoveryRetry()`

Target shape:

```cpp
while (WorkersRunning()) {
  ...
}
```

and:

```cpp
submitCv_.wait(lock, [this] {
  return !WorkersRunning() || !submitQueue_.empty();
});
```

**Step 2: Remove or shrink `shuttingDown`**

If `shuttingDown` becomes redundant after `ApiLifecycleState::Closing` and `WorkerRunState::Stopping`, delete it. If one remaining path still needs it, rename the helper around that path so the reason is explicit.

**Step 3: Run fault-injection tests**

Run: `tools/build_and_test.sh --test-regex 'rpc_eventfd_fault_injection_test|rpc_client_timeout_watchdog_test'`

Expected: worker wakeup and error paths still unwind correctly.

**Step 4: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_eventfd_fault_injection_test.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp
git commit -m "refactor: route rpc client workers through run-state helpers"
```

### Task 6: Make submit gating read like one policy

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_api_test.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`

**Step 1: Add a single helper for admission before enqueue**

Add:

```cpp
StatusCode AdmissionStatusForInvoke() const;
RpcFuture RejectInvoke(RpcCall&& call, StatusCode status);
```

`AdmissionStatusForInvoke()` should answer only:
- `Ok`
- `ClientClosed`
- `PeerDisconnected`

It must not inspect recovery lifecycle; enqueue-time recovery is still handled later by `SubmitWorker`.

**Step 2: Simplify `InvokeAsync()`**

Target shape:

```cpp
std::lock_guard<std::mutex> lock(submitMutex_);
const StatusCode admission = AdmissionStatusForInvoke();
if (admission != StatusCode::Ok) {
  return RejectInvoke(std::move(call), admission);
}
EnqueueSubmit(...);
```

**Step 3: Run API and external recovery tests**

Run: `tools/build_and_test.sh --test-regex 'rpc_client_api_test|rpc_client_external_recovery_test'`

Expected: terminal shutdown still returns `ClientClosed`, recovery cooldown behavior remains unchanged.

**Step 4: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_api_test.cpp memrpc/tests/rpc_client_external_recovery_test.cpp
git commit -m "refactor: centralize rpc client invoke admission policy"
```

### Task 7: Clean up naming and member grouping inside `Impl`

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`

**Step 1: Group fields by ownership**

Reorder the member list into these blocks:
- session + callbacks
- recovery + policy
- submission + pending
- worker coordination
- runtime counters

Add one-line block comments only where the grouping is not obvious.

**Step 2: Rename state fields for clarity**

Examples:

```cpp
std::atomic<ApiLifecycleState> apiState_{ApiLifecycleState::Open};
std::atomic<WorkerRunState> workerState_{WorkerRunState::NotStarted};
```

Avoid names like `closed_` or `running_` once the new states exist.

**Step 3: Remove dead transitional helpers**

Delete:
- old direct wrappers around `clientClosed_`
- old direct wrappers around `running_`
- `shuttingDown` if no longer needed

**Step 4: Run a full focused client suite**

Run: `tools/build_and_test.sh --test-regex 'rpc_client|engine_death_handler|rpc_eventfd_fault_injection'`

Expected: 8/8 pass.

**Step 5: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp
git commit -m "refactor: clarify rpc client internal state ownership"
```

### Task 8: Final verification and regression sweep

**Files:**
- Modify: none expected
- Test: `memrpc/tests/rpc_client_api_test.cpp`
- Test: `memrpc/tests/rpc_client_shutdown_race_test.cpp`
- Test: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Test: `memrpc/tests/engine_death_handler_test.cpp`
- Test: `memrpc/tests/rpc_eventfd_fault_injection_test.cpp`

**Step 1: Run the standard focused suite**

Run: `tools/build_and_test.sh --test-regex 'rpc_client|engine_death_handler|rpc_eventfd_fault_injection'`

Expected: all focused lifecycle/recovery/fault tests pass.

**Step 2: Run a repeated race test**

Run: `tools/build_and_test.sh --repeat until-fail:50 --test-regex rpc_client_shutdown_race_test`

Expected: no hangs, no flaky failures.

**Step 3: Review the remaining invariants manually**

Confirm these are true in code:
- `ApiLifecycleState` never drives recovery logic
- `WorkerRunState` never replaces `ClientLifecycleState`
- `Shutdown()` is idempotent
- `InvokeAsync()` cannot enqueue after shutdown begins
- worker wakeups do not depend on stale state

**Step 4: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_api_test.cpp memrpc/tests/rpc_client_shutdown_race_test.cpp memrpc/tests/rpc_client_external_recovery_test.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp memrpc/tests/engine_death_handler_test.cpp memrpc/tests/rpc_eventfd_fault_injection_test.cpp
git commit -m "refactor: model rpc client runtime state explicitly"
```
