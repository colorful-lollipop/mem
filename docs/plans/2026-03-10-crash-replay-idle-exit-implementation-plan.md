# Crash Replay Classification And Idle Exit Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add crash replay classification fields, client-side async timeout watchdog, and client-side idle reminder callbacks (no auto-exit).

**Architecture:** Extend `RpcFailure` with replay hint + runtime state + timeout stage. Implement a client watchdog that uses `enqueue_mono_ms`/`start_exec_mono_ms` to fire timeouts, and add request_id validation so late responses are dropped safely. Add a client idle reminder callback driven by the same watchdog loop, with compile-time intervals. Server only reacts to explicit `CloseSession()` (no server-side idle logic).

**Tech Stack:** C++17, GTest, POSIX shm/eventfd, existing memrpc framework.

---

### Task 1: Add Replay Hint And Timeout Stage To Public API

**Files:**
- Modify: `include/memrpc/core/types.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`

**Step 1: Write the failing test**

Add assertions in `tests/memrpc/rpc_client_api_test.cpp`:

```cpp
TEST(RpcClientApiTest, FailureCallbackFiresOnAdmissionFailure) {
  MemRpc::RpcClient client;
  MemRpc::RpcFailure captured{};
  client.SetFailureCallback([&](const MemRpc::RpcFailure& failure) {
    captured = failure;
  });

  MemRpc::RpcCall call;
  call.opcode = MemRpc::Opcode::ScanFile;
  auto future = client.InvokeAsync(call);
  MemRpc::RpcReply reply;
  future.Wait(&reply);

  EXPECT_EQ(captured.replay_hint, MemRpc::ReplayHint::Unknown);
  EXPECT_EQ(captured.last_runtime_state, MemRpc::RpcRuntimeState::Unknown);
  EXPECT_NE(captured.stage, MemRpc::FailureStage::Timeout);
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_api_test`
Expected: FAIL (unknown symbols `ReplayHint` / `RpcRuntimeState` / `Timeout`).

**Step 3: Write minimal implementation**

Update `include/memrpc/core/types.h`:
- Add `enum class ReplayHint { Unknown = 0, SafeToReplay = 1, MaybeExecuted = 2 };`
- Add `enum class RpcRuntimeState { Unknown = 0, Free, Admitted, Queued, Executing, Responding, Ready, Consumed };`

Update `include/memrpc/client/rpc_client.h`:
- Extend `FailureStage` with `Timeout`.
- Extend `RpcFailure` with:
  - `ReplayHint replay_hint = ReplayHint::Unknown;`
  - `RpcRuntimeState last_runtime_state = RpcRuntimeState::Unknown;`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_api_test`
Expected: PASS.

**Step 5: Commit**

```bash
git add include/memrpc/core/types.h include/memrpc/client/rpc_client.h tests/memrpc/rpc_client_api_test.cpp
git commit -m "feat: add replay hints and timeout stage to failures"
```

---

### Task 2: Classify Replay Hints On Session Death

**Files:**
- Create: `src/client/replay_classifier.h`
- Modify: `src/client/rpc_client.cpp`
- Create: `tests/memrpc/replay_classifier_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/memrpc/replay_classifier_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "core/protocol.h"
#include "client/replay_classifier.h"

namespace memrpc {

TEST(ReplayClassifierTest, MapsRuntimeStates) {
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Admitted), ReplayHint::SafeToReplay);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Queued), ReplayHint::SafeToReplay);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Executing), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Responding), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Ready), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Consumed), ReplayHint::MaybeExecuted);
  EXPECT_EQ(ClassifyReplayHint(SlotRuntimeStateCode::Free), ReplayHint::MaybeExecuted);
}

}  // namespace memrpc
```

Add to `tests/memrpc/CMakeLists.txt`:

```cmake
memrpc_add_test(replay_classifier_test replay_classifier_test.cpp)
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R replay_classifier_test`
Expected: FAIL (missing header / symbols).

**Step 3: Write minimal implementation**

Create `src/client/replay_classifier.h`:

```cpp
#pragma once

#include "core/protocol.h"
#include "memrpc/core/types.h"

namespace memrpc {

inline RpcRuntimeState ToRpcRuntimeState(SlotRuntimeStateCode state) {
  switch (state) {
    case SlotRuntimeStateCode::Free: return RpcRuntimeState::Free;
    case SlotRuntimeStateCode::Admitted: return RpcRuntimeState::Admitted;
    case SlotRuntimeStateCode::Queued: return RpcRuntimeState::Queued;
    case SlotRuntimeStateCode::Executing: return RpcRuntimeState::Executing;
    case SlotRuntimeStateCode::Responding: return RpcRuntimeState::Responding;
    case SlotRuntimeStateCode::Ready: return RpcRuntimeState::Ready;
    case SlotRuntimeStateCode::Consumed: return RpcRuntimeState::Consumed;
  }
  return RpcRuntimeState::Unknown;
}

inline ReplayHint ClassifyReplayHint(SlotRuntimeStateCode state) {
  switch (state) {
    case SlotRuntimeStateCode::Admitted:
    case SlotRuntimeStateCode::Queued:
      return ReplayHint::SafeToReplay;
    case SlotRuntimeStateCode::Executing:
    case SlotRuntimeStateCode::Responding:
    case SlotRuntimeStateCode::Ready:
    case SlotRuntimeStateCode::Consumed:
    case SlotRuntimeStateCode::Free:
      return ReplayHint::MaybeExecuted;
  }
  return ReplayHint::Unknown;
}

}  // namespace memrpc
```

Update `src/client/rpc_client.cpp`:
- Include `client/replay_classifier.h`.
- Extend `PendingInfo` with `ReplayHint replay_hint` + `RpcRuntimeState last_runtime_state`.
- Initialize both to `Unknown` in `MakePendingInfo`.
- In `HandleEngineDeath`, before `session.Reset()`, iterate `pending_info_slots`:
  - For each slot with pending info, read `SlotPayload* payload = session.slot_payload(i)`.
  - If payload exists, read `payload->runtime.state` and write:
    - `info.last_runtime_state = ToRpcRuntimeState(state);`
    - `info.replay_hint = ClassifyReplayHint(state);`
  - If payload missing, keep defaults.
- In `NotifyFailure`, copy `replay_hint` and `last_runtime_state` into `RpcFailure`.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R replay_classifier_test`
Expected: PASS.

**Step 5: Commit**

```bash
git add src/client/replay_classifier.h src/client/rpc_client.cpp tests/memrpc/replay_classifier_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: classify crash replay hints from slot state"
```

---

### Task 3: Add Client Async Timeout Watchdog + Late Response Drop

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `include/memrpc/client/rpc_client.h`
- Create: `tests/memrpc/rpc_client_timeout_watchdog_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/memrpc/rpc_client_timeout_watchdog_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace memrpc {

TEST(RpcClientTimeoutWatchdogTest, TriggersTimeoutAndFailureCallback) {
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  BootstrapHandles handles;
  ASSERT_EQ(bootstrap->OpenSession(&handles), StatusCode::Ok);

  RpcServer server(handles);
  server.RegisterHandler(Opcode::ScanFile,
                         [](const RpcServerCall& call, RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           std::this_thread::sleep_for(std::chrono::milliseconds(50));
                           reply->status = StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), StatusCode::Ok);

  RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), StatusCode::Ok);

  std::atomic<bool> got_failure{false};
  client.SetFailureCallback([&](const RpcFailure& failure) {
    got_failure.store(true);
    EXPECT_EQ(failure.stage, FailureStage::Timeout);
    EXPECT_EQ(failure.status, StatusCode::ExecTimeout);
  });

  RpcCall call;
  call.opcode = Opcode::ScanFile;
  call.queue_timeout_ms = 10;
  call.exec_timeout_ms = 10;
  auto future = client.InvokeAsync(call);

  RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), StatusCode::ExecTimeout);
  EXPECT_TRUE(got_failure.load());

  client.Shutdown();
  server.Stop();
}

}  // namespace memrpc
```

Add to `tests/memrpc/CMakeLists.txt`:

```cmake
memrpc_add_test(rpc_client_timeout_watchdog_test rpc_client_timeout_watchdog_test.cpp)
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R rpc_client_timeout_watchdog_test`
Expected: FAIL (no watchdog, stage not Timeout).

**Step 3: Write minimal implementation**

Update `src/client/rpc_client.cpp`:
- Add compile-time constants:
  - `constexpr uint32_t kAsyncWatchdogIntervalMs = MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS;`
  - with default (e.g. 10ms) if macro unset.
- Add a watchdog thread in `RpcClient::Impl` (start in `Init`, stop in `Shutdown`).
- Watchdog loop every `kAsyncWatchdogIntervalMs`:
  - For each pending slot with `PendingInfo`, read slot runtime state/timestamps from shm:
    - `Admitted/Queued`: if `queue_timeout_ms > 0` and `now - enqueue_mono_ms >= queue_timeout_ms` → timeout.
    - `Executing/Responding/Ready`: if `exec_timeout_ms > 0` and `now - start_exec_mono_ms >= exec_timeout_ms` → timeout.
  - On timeout:
    - Set `info.replay_hint` / `info.last_runtime_state` from shm state.
    - Call `NotifyFailure(info, StatusCode::QueueTimeout|ExecTimeout, FailureStage::Timeout)`.
    - Resolve future with same status.
    - Clear pending slot and release request slot.

- Add request_id validation in response handling:
  - When completing a response, check `pending_info.request_id == entry.request_id`.
  - If mismatch or no pending, drop response and **do not** release request slot (only release response slot).

- Ensure watchdog and response loop are synchronized with `session_mutex` to avoid races on session reset.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R rpc_client_timeout_watchdog_test`
Expected: PASS.

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp include/memrpc/client/rpc_client.h tests/memrpc/rpc_client_timeout_watchdog_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: add client async timeout watchdog"
```

---

### Task 4: Add Client Idle Reminder Callback

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Create: `tests/memrpc/rpc_client_idle_callback_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/memrpc/rpc_client_idle_callback_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"

namespace memrpc {

TEST(RpcClientIdleCallbackTest, FiresWhileIdle) {
  auto bootstrap = std::make_shared<PosixDemoBootstrapChannel>();
  RpcClient client(bootstrap);
  std::atomic<int> idle_hits{0};
  client.SetIdleCallback([&](uint64_t idle_ms) {
    if (idle_ms > 0) {
      idle_hits.fetch_add(1);
    }
  });

  ASSERT_EQ(client.Init(), StatusCode::Ok);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < deadline && idle_hits.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(idle_hits.load(), 0);
  client.Shutdown();
}

}  // namespace memrpc
```

Add to `tests/memrpc/CMakeLists.txt`:

```cmake
memrpc_add_test(rpc_client_idle_callback_test rpc_client_idle_callback_test.cpp)
```

Add compile definitions for this test target in `tests/memrpc/CMakeLists.txt`:

```cmake
target_compile_definitions(rpc_client_idle_callback_test PRIVATE
  MEMRPC_IDLE_TIMEOUT_MS=50
  MEMRPC_IDLE_NOTIFY_INTERVAL_MS=10
)
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R rpc_client_idle_callback_test`
Expected: FAIL (missing API / no callback).

**Step 3: Write minimal implementation**

Update `include/memrpc/client/rpc_client.h`:
- Add `using RpcIdleCallback = std::function<void(uint64_t idle_ms)>;`
- Add `void SetIdleCallback(RpcIdleCallback callback);`

Update `src/client/rpc_client.cpp`:
- Track `last_activity_mono_ms` and update on:
  - Session attach
  - Submit success
  - Response completion
- In watchdog loop, if `now - last_activity >= kIdleTimeoutMs`:
  - Fire `idle_callback(idle_ms)` every `kIdleNotifyIntervalMs`.
- Compile-time constants in `rpc_client.cpp`:
  - `constexpr uint32_t kIdleTimeoutMs = MEMRPC_IDLE_TIMEOUT_MS;`
  - `constexpr uint32_t kIdleNotifyIntervalMs = MEMRPC_IDLE_NOTIFY_INTERVAL_MS;`
  - Defaults when macros unset.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R rpc_client_idle_callback_test`
Expected: PASS.

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_idle_callback_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: add client idle reminder callback"
```

---

### Task 5: Update Docs

**Files:**
- Modify: `docs/architecture.md`

**Step 1: Write the doc change**

Add a short section under “恢复语义” or a new “超时与空闲提醒” section:
- Document `FailureStage::Timeout` and async watchdog behavior.
- Document idle reminder callback and compile-time constants.

**Step 2: Commit**

```bash
git add docs/architecture.md
git commit -m "docs: document async watchdog and idle reminders"
```

---

Plan complete and saved to `docs/plans/2026-03-10-crash-replay-idle-exit-implementation-plan.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
