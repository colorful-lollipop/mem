# MemRPC Recovery Policy Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace per-callback failure/idle/death handlers with a unified `RecoveryPolicy`, and add forced restart on exec-timeout in VpsClient.

**Architecture:** `RpcClient` owns a `RecoveryPolicy` with three handlers returning `RecoveryDecision`. Failures and idle notifications can request a forced restart that calls `CloseSession`, waits, re-opens, and replays safe calls. Engine-death continues to use the existing replay snapshot but now consults the policy for restart decisions.

**Tech Stack:** C++17, memrpc framework, GoogleTest

---

### Task 1: Add Public RecoveryPolicy API (Header + Compile-Fail Test)

**Files:**
- Create: `tests/memrpc/rpc_client_recovery_policy_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

#include "memrpc/client/rpc_client.h"

TEST(RpcClientRecoveryPolicyTest, PolicyCanBeSet) {
  memrpc::RpcClient client;
  memrpc::RecoveryPolicy policy;
  policy.onFailure = [](const memrpc::RpcFailure&) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
}
```

Add to `tests/memrpc/CMakeLists.txt`:

```cmake
memrpc_add_test(memrpc_rpc_client_recovery_policy_test rpc_client_recovery_policy_test.cpp)
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build`

Expected: compile failure (missing `RecoveryPolicy` / `SetRecoveryPolicy`).

**Step 3: Commit (test only)**

```bash
git add tests/memrpc/rpc_client_recovery_policy_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "feat: add recovery policy api test"
```

---

### Task 2: Implement RecoveryPolicy Types and SetRecoveryPolicy

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `include/memrpc/client/typed_future.h` (if needed for signatures)

**Step 1: Add public API types (header)**

```cpp
enum class RecoveryAction { Ignore, Restart };

struct RecoveryDecision {
  RecoveryAction action = RecoveryAction::Ignore;
  uint32_t delay_ms = 0;
};

struct RecoveryPolicy {
  std::function<RecoveryDecision(const RpcFailure&)> onFailure;
  std::function<RecoveryDecision(uint64_t idle_ms)> onIdle;
  std::function<RecoveryDecision(const EngineDeathReport&)> onEngineDeath;
  uint32_t idle_timeout_ms = 0;
  uint32_t idle_notify_interval_ms = 0;
};
```

Replace setters on `RpcClient` / `RpcSyncClient`:

```cpp
void SetRecoveryPolicy(RecoveryPolicy policy);
```

**Step 2: Implement storage + plumbing in RpcClient**

- Replace `failure_callback`, `idle_callback`, `death_handler` with a single `RecoveryPolicy` + mutex.
- Implement `SetRecoveryPolicy` to store policy and update idle timeouts.
- Update `RpcSyncClient` to forward `SetRecoveryPolicy`.

**Step 3: Run build to verify test compiles**

Run: `cmake --build build`

Expected: build succeeds; new test compiles.

**Step 4: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp
git commit -m "feat: add recovery policy api"
```

---

### Task 3: Forced Restart Path for Failure / Idle

**Files:**
- Modify: `src/client/rpc_client.cpp`

**Step 1: Add restart gating + intentional close guard**

Add members:

```cpp
std::atomic<bool> restart_pending{false};
std::atomic<bool> suppress_death_callback{false};
```

**Step 2: Implement forced restart helper**

```cpp
void RequestRestart(const RecoveryDecision& decision, RestartCause cause);
void RestartWithSnapshot(uint32_t delay_ms, ReplayableSnapshot snapshot, bool close_first);
```

- For `close_first=true`: set `suppress_death_callback=true`, call `bootstrap->CloseSession()`, reset flag.
- Reuse existing replay snapshot classification.

**Step 3: Wire onFailure / onIdle to restart**

- In `NotifyFailure`, call `policy.onFailure` (if set) and `RequestRestart`.
- In watchdog idle path, call `policy.onIdle` and `RequestRestart`.

**Step 4: Run focused test build**

Run: `cmake --build build`

Expected: build succeeds.

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp
git commit -m "feat: add forced restart path"
```

---

### Task 4: Update Engine-Death Path to RecoveryPolicy

**Files:**
- Modify: `src/client/rpc_client.cpp`

**Step 1: Replace EngineDeathHandler usage**

- Look up `policy.onEngineDeath` under the policy mutex.
- If absent, keep current behavior: fail all pending and exit.
- If present, use its `RecoveryDecision` to decide restart.

**Step 2: Ensure suppress_death_callback is respected**

- In the bootstrap death callback, return early if `suppress_death_callback` is true.

**Step 3: Run build**

Run: `cmake --build build`

Expected: build succeeds.

**Step 4: Commit**

```bash
git add src/client/rpc_client.cpp
git commit -m "feat: migrate engine-death handling to recovery policy"
```

---

### Task 5: Update Tests to New API

**Files:**
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_timeout_watchdog_test.cpp`
- Modify: `tests/memrpc/rpc_client_idle_callback_test.cpp`
- Modify: `tests/memrpc/engine_death_handler_test.cpp`

**Step 1: Update tests to SetRecoveryPolicy**

Example for failure:

```cpp
memrpc::RecoveryPolicy policy;
policy.onFailure = [&](const memrpc::RpcFailure& failure) {
  captured = failure;
  return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
};
client.SetRecoveryPolicy(std::move(policy));
```

Example for idle:

```cpp
memrpc::RecoveryPolicy policy;
policy.idle_timeout_ms = 100;
policy.idle_notify_interval_ms = 50;
policy.onIdle = [&](uint64_t idle_ms) {
  got_idle = true;
  return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
};
client.SetRecoveryPolicy(std::move(policy));
```

Example for engine death:

```cpp
memrpc::RecoveryPolicy policy;
policy.onEngineDeath = [&](const memrpc::EngineDeathReport&) {
  return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, 0};
};
client.SetRecoveryPolicy(std::move(policy));
```

**Step 2: Run tests**

Run: `ctest --test-dir build --output-on-failure`

Expected: failures only related to remaining API updates.

**Step 3: Commit**

```bash
git add tests/memrpc/rpc_client_api_test.cpp \
  tests/memrpc/rpc_client_timeout_watchdog_test.cpp \
  tests/memrpc/rpc_client_idle_callback_test.cpp \
  tests/memrpc/engine_death_handler_test.cpp
git commit -m "fix: update memrpc tests for recovery policy"
```

---

### Task 6: Add ExecTimeout Restart Test (CloseSession + Reopen)

**Files:**
- Modify: `tests/memrpc/rpc_client_timeout_watchdog_test.cpp`

**Step 1: Add a counting bootstrap wrapper**

```cpp
class CountingBootstrapChannel : public memrpc::IBootstrapChannel {
 public:
  explicit CountingBootstrapChannel(std::shared_ptr<memrpc::PosixDemoBootstrapChannel> inner)
      : inner_(std::move(inner)) {}

  memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) override {
    ++open_count;
    return inner_->OpenSession(handles);
  }

  memrpc::StatusCode CloseSession() override {
    ++close_count;
    return inner_->CloseSession();
  }

  void SetEngineDeathCallback(memrpc::EngineDeathCallback callback) override {
    inner_->SetEngineDeathCallback(std::move(callback));
  }

  std::atomic<int> open_count{0};
  std::atomic<int> close_count{0};

 private:
  std::shared_ptr<memrpc::PosixDemoBootstrapChannel> inner_;
};
```

**Step 2: Add test**

```cpp
TEST(RpcClientTimeoutWatchdogTest, ExecTimeoutTriggersForcedRestart) {
  auto inner = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  auto bootstrap = std::make_shared<CountingBootstrapChannel>(inner);
  memrpc::BootstrapHandles unused;
  ASSERT_EQ(inner->OpenSession(&unused), memrpc::StatusCode::Ok);

  memrpc::RpcServer server(inner->serverHandles());
  // ... register a handler that sleeps longer than exec_timeout_ms ...
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  memrpc::RecoveryPolicy policy;
  policy.onFailure = [&](const memrpc::RpcFailure& f) {
    if (f.status == memrpc::StatusCode::ExecTimeout) {
      return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, 10};
    }
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
  };
  client.SetRecoveryPolicy(std::move(policy));
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  // issue a call that times out
  // wait for open_count >= 2 and close_count >= 1
  EXPECT_TRUE(WaitForCondition([&] { return bootstrap->open_count >= 2; }, 2000));
}
```

**Step 3: Run the test**

Run: `ctest --test-dir build --output-on-failure -R rpc_client_timeout_watchdog_test`

Expected: PASS and open/close counts advance.

**Step 4: Commit**

```bash
git add tests/memrpc/rpc_client_timeout_watchdog_test.cpp
git commit -m "feat: test exec-timeout forced restart"
```

---

### Task 7: Update VpsClient to Use RecoveryPolicy

**Files:**
- Modify: `demo/vpsdemo/include/ves_client.h`
- Modify: `demo/vpsdemo/src/ves_client.cpp`
- Modify: `demo/vpsdemo/src/vesdemo_client.cpp` (if options need wiring)

**Step 1: Add Options to VpsClient**

```cpp
struct VpsClientOptions {
  uint32_t execTimeoutRestartDelayMs = 200;
  uint32_t engineDeathRestartDelayMs = 200;
  uint32_t idleRestartDelayMs = 0;
};
```

**Step 2: Apply policy in VpsClient::Init**

```cpp
memrpc::RecoveryPolicy policy;
policy.onFailure = [delay = options_.execTimeoutRestartDelayMs](const memrpc::RpcFailure& failure) {
  if (failure.status == memrpc::StatusCode::ExecTimeout) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, delay};
  }
  return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
};
policy.onEngineDeath = [delay = options_.engineDeathRestartDelayMs](const memrpc::EngineDeathReport& report) {
  // log report
  return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, delay};
};
if (options_.idleRestartDelayMs > 0) {
  policy.onIdle = [delay = options_.idleRestartDelayMs](uint64_t) {
    return memrpc::RecoveryDecision{memrpc::RecoveryAction::Restart, delay};
  };
}
client_.SetRecoveryPolicy(std::move(policy));
```

**Step 3: Run build**

Run: `cmake --build build`

Expected: build succeeds.

**Step 4: Commit**

```bash
git add demo/vpsdemo/include/ves_client.h demo/vpsdemo/src/ves_client.cpp demo/vpsdemo/src/vesdemo_client.cpp
git commit -m "feat: configure vps client recovery policy"
```

---

### Task 8: Full Test Run

**Files:**
- None

**Step 1: Run full tests**

Run: `ctest --test-dir build --output-on-failure`

Expected: all tests pass.

**Step 2: Final commit if any remaining updates**

```bash
git add -A
git commit -m "fix: finalize recovery policy integration"
```
