# Client Wait Timeout Watchdog Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a client-side pending-request watchdog so that once a request has been successfully pushed into the request ring, the waiting caller can fail with `ExecTimeout` after a configured timeout, and any later real response is ignored.

**Architecture:** Keep the change entirely on the client side. Reuse the existing `RpcClient` pending map and watchdog thread to track per-request deadlines after successful request-ring admission. When a deadline expires, remove that request from `pending_`, resolve its future with `ExecTimeout`, and let late responses be naturally discarded because `ResolveCompletedRequest()` can no longer find the request ID.

**Tech Stack:** C++17, `memrpc/src/client/rpc_client.cpp`, GoogleTest, existing `RpcClient` watchdog / submit / response threads.

---

## Background

The current codebase has three timeout concepts:

- `admissionTimeoutMs`: client-side wait for request-ring capacity before the request is actually published.
- `queueTimeoutMs`: server-side timeout while the request waits in the server queue.
- `execTimeoutMs`: currently implemented as a post-hoc soft timeout on the server, which only changes the final status if the handler eventually returns.

Today, synchronous callers (`RpcSyncClient::InvokeSync`) ultimately wait through `RpcFuture::Wait()`, which blocks indefinitely until the future becomes ready. There is no built-in client-side deadline for the interval between "request published to the ring" and "final response received".

That creates an operational problem:

- if the request is published successfully,
- but the response never arrives,
- the waiting thread can block forever.

For the selected simplified design, we are intentionally **not** trying to detect whether the server has started execution. The client does not currently have a reliable "request dequeued / execution started" signal. The simplest workable boundary is:

- start the timeout **after successful `PushRequest()`**
- end the timeout when the client resolves the request with either a real response or a synthetic timeout

This means the effective semantics become **client wait timeout after request publication**, while still surfacing as `StatusCode::ExecTimeout` for compatibility.

## Purpose

This design is meant to solve one concrete problem quickly and safely:

- unblock waiting threads in finite time
- keep the implementation small and local
- avoid protocol changes
- avoid adding server-side active-execution state
- make late replies harmless by discarding them on the client

## Non-Goals

This plan explicitly does **not** do the following:

- it does not distinguish "still queued" from "already executing"
- it does not force-stop or cancel a running handler
- it does not add a new protocol message for server execution state
- it does not change shared-memory layout or protocol version
- it does not redesign `queueTimeoutMs`

## Chosen Semantics

For this implementation, use the following concrete rule:

1. `admissionTimeoutMs` continues to cover only the period before the request is successfully pushed into the request ring.
2. After `TryPushRequest()` succeeds, if `call.execTimeoutMs > 0`, record a client-side deadline.
3. While the request remains in `pending_`, the client watchdog checks whether the deadline has passed.
4. Once the deadline passes, the client removes the request from `pending_` and resolves the future with `StatusCode::ExecTimeout`.
5. If the server later publishes the real response, `ResolveCompletedRequest()` finds no pending entry and drops it.

This is the desired tradeoff:

- simple
- deterministic
- no protocol work
- enough to stop indefinite blocking

The cost is that `ExecTimeout` now means "published request waited too long for a final response" rather than "server execution duration alone exceeded the threshold".

## Design Details

### Timeout Start Point

Start timing only after `session_.PushRequest(...)` succeeds in `TryPushRequest()`.

Do **not** start timing:

- when the request is added to `submitQueue_`
- while waiting for request credit
- before session establishment succeeds

Reason:

- before `PushRequest()` succeeds, the request is still in admission flow
- starting earlier would overlap with `admissionTimeoutMs`
- a request that was never published should not consume the wait-timeout budget

### Pending State Extensions

Extend `PendingRequest` to carry enough state for watchdog decisions:

```cpp
struct PendingRequest {
  std::shared_ptr<RpcFuture::State> future;
  PendingInfo info;
  std::chrono::steady_clock::time_point waitDeadline =
      std::chrono::steady_clock::time_point::max();
  uint64_t admittedMonoMs = 0;
};
```

Definitions:

- `waitDeadline`: `now + execTimeoutMs` when `execTimeoutMs > 0`, otherwise `time_point::max()`
- `admittedMonoMs`: optional debug field for observability and assertions; record `MonotonicNowMs64()` at successful publish time

### Watchdog Behavior

Add a new helper in `RpcClient::Impl`:

```cpp
void MaybeRunPendingTimeouts();
```

Behavior:

- lock `pendingMutex_`
- collect all expired `PendingRequest` entries into a local vector
- erase them from `pending_` while still under the lock
- unlock
- for each expired request:
  - call `FailAndResolve(info, StatusCode::ExecTimeout, FailureStage::Timeout, future)`

This separation is important:

- erase under lock so late responses cannot race in and also complete the same future through `pending_`
- resolve outside the map lock to avoid lock inversion / callback hazards

### Late Reply Handling

No extra late-reply protocol handling is required.

Current code already does this in `ResolveCompletedRequest()`:

- look up `entry.requestId` in `pending_`
- if not found, return immediately

That is exactly what we want after a timeout:

- watchdog removes the request from `pending_`
- late response arrives
- response thread cannot find the request
- response is ignored

### Race Model

There are two relevant racing completions:

1. response thread receives a real reply first
2. watchdog reaches the timeout first

The arbitration mechanism is simply the `pending_` map:

- whichever path removes the `requestId` from `pending_` first wins
- the loser sees "not found" and does nothing

To keep this safe, both code paths must follow the same rule:

- remove from `pending_` while holding `pendingMutex_`
- only resolve after ownership is established by successful removal

### Failure Classification

Keep the surfaced status as:

- `StatusCode::ExecTimeout`
- `FailureStage::Timeout`
- `ReplayHint::MaybeExecuted`

Reason:

- after publication, the client no longer knows whether the request was still queued, already executing, or even fully executed but blocked on response delivery
- `MaybeExecuted` is the only safe replay hint

### Interaction With Existing Watchdog Work

The current `WatchdogLoop()` already runs periodically and does:

- `MaybeRunHealthCheck()`
- `MaybeRunIdlePolicy()`

Add `MaybeRunPendingTimeouts()` in the same loop.

Recommended order:

1. `MaybeRunPendingTimeouts()`
2. `MaybeRunHealthCheck()`
3. `MaybeRunIdlePolicy()`

Reason:

- expired pending waiters should be released promptly
- idle policy already skips execution when `pending_` is non-empty
- health checks can remain as they are

### API / Comment Semantics

We need to be explicit in comments because the selected design intentionally repurposes current semantics.

Update `RpcCall::execTimeoutMs` comments to say:

- the timeout is enforced from successful client-side request publication until a final reply is observed
- the framework does not cancel server execution
- late replies are ignored after timeout

Do **not** leave the old "server handler execution stage soft timeout" wording in place, because that will become misleading after this change.

### Optional Minimal Observability

Add small runtime counters in `RpcClientRuntimeStats` only if the implementation stays clean:

- `timedOutPendingCalls`
- `lateRepliesDropped`

This is optional. If it creates too much API churn, skip it for the first patch.

---

### Task 1: Write focused failing tests for client-side wait timeout

**Files:**
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Add a test that proves the waiter fails before the slow handler returns**

Use a slow handler and assert that `Wait()` returns `ExecTimeout` well before handler completion:

```cpp
TEST(RpcClientTimeoutWatchdogTest, ClientWaitTimeoutUnblocksWaiterBeforeSlowReplyArrives) {
  auto bootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(bootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  std::atomic<bool> handlerEntered{false};
  std::atomic<bool> handlerExited{false};

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestEchoOpcode, [&](const RpcServerCall&, RpcServerReply* reply) {
    handlerEntered.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    handlerExited.store(true);
    reply->status = StatusCode::Ok;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  RpcCall call;
  call.opcode = kTestEchoOpcode;
  call.queueTimeoutMs = 5000;
  call.execTimeoutMs = 50;

  const auto start = std::chrono::steady_clock::now();
  auto future = client.InvokeAsync(call);

  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::ExecTimeout);
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_TRUE(handlerEntered.load());
  EXPECT_LT(elapsedMs, 200);
  EXPECT_FALSE(handlerExited.load());

  client.Shutdown();
  server.Stop();
}
```

**Step 2: Add a test that proves a late reply is discarded**

After the first timed-out call, wait for the slow handler to finish, then issue a second request and assert only the second reply is consumed:

```cpp
TEST(RpcClientTimeoutWatchdogTest, LateReplyAfterClientWaitTimeoutIsDiscarded) {
  auto bootstrap = std::make_shared<DevBootstrapChannel>();
  BootstrapHandles unusedHandles;
  ASSERT_EQ(bootstrap->OpenSession(unusedHandles), StatusCode::Ok);
  CloseHandles(unusedHandles);

  std::atomic<int> callCount{0};

  RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  server.RegisterHandler(kTestEchoOpcode, [&](const RpcServerCall&, RpcServerReply* reply) {
    const int current = ++callCount;
    if (current == 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    reply->status = StatusCode::Ok;
    reply->errorCode = current;
  });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  RpcCall first;
  first.opcode = kTestEchoOpcode;
  first.queueTimeoutMs = 5000;
  first.execTimeoutMs = 50;
  auto firstFuture = client.InvokeAsync(first);

  RpcReply firstReply;
  EXPECT_EQ(firstFuture.Wait(&firstReply), StatusCode::ExecTimeout);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  RpcCall second;
  second.opcode = kTestEchoOpcode;
  second.queueTimeoutMs = 5000;
  second.execTimeoutMs = 500;
  auto secondFuture = client.InvokeAsync(second);

  RpcReply secondReply;
  EXPECT_EQ(secondFuture.Wait(&secondReply), StatusCode::Ok);
  EXPECT_EQ(secondReply.errorCode, 2);

  client.Shutdown();
  server.Stop();
}
```

**Step 3: Keep the existing tests that verify queue timeout and health watchdog behavior**

Do not delete or weaken:

- `TriggersQueueTimeoutWhenStuckInQueue`
- `HealthFailuresTriggerWatchdogRestart`

They protect unrelated timeout / recovery semantics.

**Step 4: Run the focused test binary to confirm failure first**

Run: `tools/build_and_test.sh --test-regex memrpc_rpc_client_timeout_watchdog_test`

Expected: FAIL because current client code does not proactively expire pending requests.

**Step 5: Commit**

```bash
git add memrpc/tests/rpc_client_timeout_watchdog_test.cpp
git commit -m "test: lock client wait timeout watchdog behavior"
```

### Task 2: Extend pending request state with a client-side deadline

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Extend `PendingRequest`**

Modify `RpcClient::Impl::PendingRequest` in `memrpc/src/client/rpc_client.cpp` to include:

```cpp
std::chrono::steady_clock::time_point waitDeadline =
    std::chrono::steady_clock::time_point::max();
uint64_t admittedMonoMs = 0;
```

**Step 2: Set the deadline only after request publication succeeds**

In `TryPushRequest(...)`, move the pending insertion logic so it records the deadline using the current time immediately before or after a successful push. The simplest safe pattern is:

1. build `RequestRingEntry`
2. attempt `session_.PushRequest(...)`
3. if push succeeds:
   - build `PendingRequest`
   - set `waitDeadline`
   - insert into `pending_`
4. signal request eventfd if needed

If that exact ordering makes the response path racy because the server could theoretically reply before `pending_` insertion, then use this safer variant:

1. build `PendingRequest`
2. precompute `waitDeadline`
3. insert into `pending_`
4. attempt `PushRequest`
5. erase on push failure

In that variant, the start time must still be recorded as the moment immediately before the actual `PushRequest`, not earlier in `SubmitOne`.

**Step 3: Add a helper for deadline computation**

Add:

```cpp
std::chrono::steady_clock::time_point MakePendingWaitDeadline(uint32_t execTimeoutMs) const {
  if (execTimeoutMs == 0) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + std::chrono::milliseconds(execTimeoutMs);
}
```

**Step 4: Keep `PendingInfo` unchanged**

Do not add queue/execution-state guesses. The client does not know them reliably.

**Step 5: Run the focused test again**

Run: `tools/build_and_test.sh --test-regex memrpc_rpc_client_timeout_watchdog_test`

Expected: still FAIL because the watchdog does not yet scan the new deadline.

**Step 6: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/include/memrpc/client/rpc_client.h
git commit -m "feat: track client-side pending wait deadlines"
```

### Task 3: Add pending-timeout scanning to the client watchdog

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Add `MaybeRunPendingTimeouts()`**

Implement:

```cpp
void MaybeRunPendingTimeouts() {
  std::vector<PendingRequest> expired;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.waitDeadline != std::chrono::steady_clock::time_point::max() &&
          now >= it->second.waitDeadline) {
        expired.push_back(std::move(it->second));
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& pending : expired) {
    FailAndResolve(pending.info, StatusCode::ExecTimeout, FailureStage::Timeout, pending.future);
  }
}
```

**Step 2: Call it from `WatchdogLoop()`**

Update the loop:

```cpp
void WatchdogLoop() {
  while (running_.load(std::memory_order_acquire)) {
    MaybeRunPendingTimeouts();
    MaybeRunHealthCheck();
    MaybeRunIdlePolicy();
    ...
  }
}
```

**Step 3: Preserve existing polling cadence**

Do not add a second watchdog thread. Reuse the existing cadence:

- `kHealthCheckPeriod`
- `kIdlePollPeriod`

If tighter timeout responsiveness is needed later, tune the existing watchdog period in a follow-up. Do not complicate this patch with a new configurable interval unless tests prove it is necessary.

**Step 4: Make timeout resolution idempotent**

Rely on `ResolveState()` already guarding against double completion through `state->ready`, but still ensure `pending_` ownership is decided before resolving.

**Step 5: Run the focused tests**

Run: `tools/build_and_test.sh --test-regex memrpc_rpc_client_timeout_watchdog_test`

Expected: PASS

**Step 6: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp
git commit -m "feat: expire pending calls from client watchdog"
```

### Task 4: Tighten the race handling and late-reply behavior

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Test: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Refactor `ResolveCompletedRequest()` to use ownership-by-erasure**

Keep the current pattern, but make the intended ownership semantics explicit in comments:

```cpp
// pending_ ownership decides terminal completion. If the watchdog has already
// erased this request, the real reply is late and must be ignored.
```

**Step 2: Verify timeout and response races**

Review these race windows carefully:

- watchdog erases from `pending_`, then response arrives
- response erases from `pending_`, then watchdog scans
- shutdown fails all pending while watchdog is scanning

The implementation must ensure:

- only one path removes a given entry from `pending_`
- resolution is always performed after ownership is decided
- no callback or `cv.notify_all()` happens while holding `pendingMutex_`

**Step 3: Add a test for race stability if feasible**

If a deterministic test can be written cheaply, add one using repeated iterations:

```cpp
TEST(RpcClientTimeoutWatchdogTest, LateReplyRaceDoesNotDoubleResolve) {
  for (int i = 0; i < 50; ++i) {
    // issue a request with timeout near handler completion
    // verify exactly one terminal result is observed and no crash occurs
  }
}
```

If a deterministic assertion is too brittle, skip this step and document the reason in the final summary.

**Step 4: Run the focused suite**

Run: `tools/build_and_test.sh --test-regex memrpc_rpc_client_timeout_watchdog_test`

Expected: PASS

**Step 5: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp
git commit -m "fix: harden client wait timeout race handling"
```

### Task 5: Update comments and caller-facing semantics

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/include/memrpc/client/typed_future.h`
- Test: `memrpc/tests/rpc_client_api_test.cpp`

**Step 1: Fix the `execTimeoutMs` comment**

Replace the old comment:

```cpp
// exec_timeout_ms 作用在服务端 handler 执行阶段；当前为软超时，不强杀 handler。
```

with wording that matches the selected design:

```cpp
// exec_timeout_ms limits how long the client waits for a final reply after the
// request has been successfully published to the request ring. It does not
// cancel server execution; late replies are ignored after timeout.
```

**Step 2: Clarify `WaitFor()` comments if needed**

`RpcFuture::WaitFor()` is still an explicit caller-provided wait timeout and is different from the internal watchdog timeout. Add a short clarifying note if the code reads ambiguously.

**Step 3: Add a small API-level test if helpful**

If there is a simple place to assert the comment-level semantics indirectly, add a focused test in `memrpc/tests/rpc_client_api_test.cpp`. If not, rely on the timeout watchdog tests.

**Step 4: Run the API-related focused tests**

Run: `tools/build_and_test.sh --test-regex "memrpc_rpc_client_timeout_watchdog_test|memrpc_rpc_client_api_test"`

Expected: PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h memrpc/include/memrpc/client/typed_future.h memrpc/tests/rpc_client_api_test.cpp
git commit -m "docs: align exec timeout comments with client watchdog behavior"
```

### Task 6: Run repo-appropriate verification for a client concurrency change

**Files:**
- Test: `tools/build_and_test.sh`
- Test: `tools/push_gate.sh`

**Step 1: Run the memrpc-focused verification**

Run: `tools/build_and_test.sh --test-regex memrpc`

Expected: PASS

**Step 2: Run the standard push gate**

Run: `tools/push_gate.sh`

Expected: PASS

**Step 3: If the patch shows timing sensitivity, run the deep gate**

Run: `tools/push_gate.sh --deep`

Expected: PASS

**Step 4: Record the intentional semantic tradeoff**

The final summary must explicitly say:

```text
This patch makes execTimeoutMs behave as a client-side wait timeout that starts
after successful request publication. The framework still cannot distinguish
queued-vs-executing state on the client, and it does not cancel server work.
```

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h memrpc/src/client/rpc_client.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp
git commit -m "feat: validate client wait timeout watchdog"
```

## Implementation Notes For A Zero-Context AI

- The only reliable start point for the timeout is the successful `PushRequest()` path in [rpc_client.cpp](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp#L567).
- `pending_` currently lives in [rpc_client.cpp](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp#L204) and is already the ownership point for real responses.
- The watchdog loop already exists in [rpc_client.cpp](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp#L897); reuse it instead of creating a new thread.
- Real responses are resolved in [rpc_client.cpp](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp#L690); this is where late-reply dropping already naturally happens if the request has been removed from `pending_`.
- The public comment for `execTimeoutMs` is in [rpc_client.h](/root/code/demo/mem/memrpc/include/memrpc/client/rpc_client.h#L25) and must be updated so future maintainers do not assume server-side execution-only semantics.
- Existing timeout tests live in [rpc_client_timeout_watchdog_test.cpp](/root/code/demo/mem/memrpc/tests/rpc_client_timeout_watchdog_test.cpp#L101); extend them instead of creating a brand-new file.
- Do not change shared-memory structures or protocol version for this plan.
- Do not add guessed `RpcRuntimeState` transitions. The client still does not know queued vs executing reliably.

## Acceptance Criteria

- A request that has been published to the request ring and waits longer than `execTimeoutMs` is completed on the client with `StatusCode::ExecTimeout`.
- `RpcFuture::Wait()` and `RpcSyncClient::InvokeSync()` no longer block indefinitely in that scenario.
- A late real response for a timed-out request is ignored.
- Existing queue-timeout and health-recovery tests still pass.
- The code comments reflect the new semantics accurately.
