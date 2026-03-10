# SA OpenSession/CloseSession Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace `StartEngine/Connect` with a single public `OpenSession` + `CloseSession` startup API aligned with SA, while keeping server-owned lazy session creation and updating clients/tests/demos.

**Architecture:** `IBootstrapChannel` exposes `OpenSession`/`CloseSession` plus death callback. Server owns shm/eventfd and creates them lazily on `OpenSession`. Client `RpcClient::Init()` calls `OpenSession` and attaches; `Shutdown()` calls `CloseSession`. Internal implementations may still use `EnsureServerReady` + `AcquireSessionHandles`.

**Tech Stack:** C++17, GoogleTest, CMake, POSIX shm/eventfd

---

**Note:** Execute in a dedicated worktree (per @superpowers:brainstorming guidance). Follow @superpowers:test-driven-development for each task.

### Task 1: Update Bootstrap API Surface

**Files:**
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `tests/memrpc/api_headers_test.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/sa_bootstrap_stub_test.cpp`

**Step 1: Write the failing tests**

Update fake bootstrap in tests to use the new API:

```cpp
class FakeBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
  MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles* handles) override {
    if (handles == nullptr) { return MemRpc::StatusCode::InvalidArgument; }
    *handles = MemRpc::BootstrapHandles{};
    handles->protocol_version = 1;
    handles->session_id = 1;
    return MemRpc::StatusCode::Ok;
  }
  MemRpc::StatusCode CloseSession() override { return MemRpc::StatusCode::Ok; }
  void SetEngineDeathCallback(MemRpc::EngineDeathCallback) override {}
};
```

**Step 2: Run tests to verify they fail**

Run: `cmake --build build --target memrpc_api_headers_test`
Expected: FAIL (missing `OpenSession/CloseSession` in interface).

**Step 3: Update the interface**

Modify `include/memrpc/core/bootstrap.h`:

```cpp
virtual StatusCode OpenSession(BootstrapHandles* handles) = 0;
virtual StatusCode CloseSession() = 0;
virtual void SetEngineDeathCallback(EngineDeathCallback callback) = 0;
```

Remove `StartEngine/Connect/NotifyPeerRestarted` from the public interface.

**Step 4: Run tests to verify they pass**

Run: `cmake --build build --target memrpc_api_headers_test`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/bootstrap.h tests/memrpc/api_headers_test.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/sa_bootstrap_stub_test.cpp
git commit -m "feat: rename bootstrap api to open/close session"
```

### Task 2: Implement OpenSession/CloseSession in Bootstrap Channels

**Files:**
- Modify: `include/memrpc/client/demo_bootstrap.h`
- Modify: `include/memrpc/client/sa_bootstrap.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `src/bootstrap/sa_bootstrap.cpp`

**Step 1: Write failing tests (or update existing)**

Add a simple stub test to `tests/memrpc/sa_bootstrap_stub_test.cpp`:

```cpp
memrpc::BootstrapHandles handles{};
EXPECT_EQ(channel.OpenSession(&handles), memrpc::StatusCode::Ok);
EXPECT_EQ(channel.CloseSession(), memrpc::StatusCode::Ok);
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_sa_bootstrap_stub_test`
Expected: FAIL (no OpenSession/CloseSession implementation).

**Step 3: Implement OpenSession/CloseSession**

In `PosixDemoBootstrapChannel`, implement lazy creation and idempotency:

```cpp
StatusCode OpenSession(BootstrapHandles* handles) override {
  if (handles == nullptr) { return StatusCode::InvalidArgument; }
  if (!impl_->initialized) { const StatusCode status = EnsureServerReady(); if (status != Ok) return status; }
  return DuplicateHandles(impl_->handles, handles, &impl_->dup_fail_after_count) ? Ok : EngineInternalError;
}

StatusCode CloseSession() override {
  impl_->ResetHandles();
  if (!impl_->config.shm_name.empty()) { shm_unlink(impl_->config.shm_name.c_str()); }
  return StatusCode::Ok;
}
```

In `SaBootstrapChannel`, forward to fallback.

**Step 4: Run tests to verify they pass**

Run: `cmake --build build --target memrpc_sa_bootstrap_stub_test && ctest --test-dir build --output-on-failure -R memrpc_sa_bootstrap_stub_test`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/demo_bootstrap.h include/memrpc/client/sa_bootstrap.h src/bootstrap/posix_demo_bootstrap.cpp src/bootstrap/sa_bootstrap.cpp tests/memrpc/sa_bootstrap_stub_test.cpp
git commit -m "feat: implement open/close session bootstraps"
```

### Task 3: Update RpcClient to Use OpenSession/CloseSession

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`

**Step 1: Write failing test**

Update `tests/memrpc/rpc_client_api_test.cpp` to assert `OpenSession()` is called by `Init()` (use a counting fake).

```cpp
struct CountingBootstrap : MemRpc::IBootstrapChannel {
  int open_calls = 0;
  StatusCode OpenSession(MemRpc::BootstrapHandles* handles) override { ++open_calls; *handles = {}; return Ok; }
  StatusCode CloseSession() override { return Ok; }
  void SetEngineDeathCallback(EngineDeathCallback) override {}
};
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_rpc_client_api_test`
Expected: FAIL (Init still calls old API).

**Step 3: Implement minimal changes**

In `RpcClient::Init()`:

```cpp
BootstrapHandles handles;
const StatusCode open_status = bootstrap->OpenSession(&handles);
if (open_status != StatusCode::Ok) { return open_status; }
// attach session using handles
```

In `RpcClient::Shutdown()` call `CloseSession()` when bootstrap is set.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_api_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp
git commit -m "feat: switch rpc client init to open session"
```

### Task 4: Update Call Sites (Demo + VPS)

**Files:**
- Modify: `demo/minirpc_demo_main.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`

**Step 1: Write failing build step**

Run: `cmake --build build --target memrpc_minirpc_demo`
Expected: FAIL (old StartEngine/Connect usage).

**Step 2: Update demo to new API**

```cpp
auto bootstrap = std::make_shared<Mem::PosixDemoBootstrapChannel>();
Mem::BootstrapHandles handles{};
if (bootstrap->OpenSession(&handles) != Mem::StatusCode::Ok) { ... }
```

Adjust any server startup wiring accordingly (temporary until SA mock is introduced).

**Step 3: Update VPS call site**

Replace `StartEngine()` with `OpenSession(&handles)` or `OpenSession()` on the bootstrap used in VPS manager.

**Step 4: Run build to verify**

Run: `cmake --build build --target memrpc_minirpc_demo`
Expected: PASS

**Step 5: Commit**

```bash
git add demo/minirpc_demo_main.cpp src/apps/vps/parent/virus_engine_manager.cpp
git commit -m "feat: update app call sites to open session api"
```

### Task 5: Align Architecture Docs

**Files:**
- Modify: `docs/architecture.md`

**Step 1: Update API section**

Replace mentions of `StartEngine/Connect/InvokeSync` with `OpenSession/CloseSession` and async-first client notes.

**Step 2: Run doc-only check**

Run: `rg -n "StartEngine|Connect" docs/architecture.md`
Expected: no matches

**Step 3: Commit**

```bash
git add docs/architecture.md
git commit -m "feat: align docs with open session api"
```

---

Plan complete and saved to `docs/plans/2026-03-10-sa-open-session-implementation-plan.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
