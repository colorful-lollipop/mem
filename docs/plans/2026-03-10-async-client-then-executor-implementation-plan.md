# Async-First Client Split + Then Executor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make async usage the primary client path, add an optional `Then` executor, and expose sync calls via a dedicated `RpcSyncClient` without changing wire protocol behavior.

**Architecture:** `RpcClient` remains async-only, `RpcSyncClient` wraps `RpcClient` for sync calls, and `RpcFuture::Then` accepts an optional executor to control callback dispatch. Typed helpers stay in `typed_invoker.h` to keep codec dependencies out of the core client.

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: Add executor-aware `Then` API + unit test

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`

**Step 1: Write the failing test**

Add a new test near the existing `Then` tests:

```cpp
TEST(RpcClientApiTest, ThenUsesExecutorWhenProvided) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  auto future = client.InvokeAsync(MemRpc::RpcCall{});
  ASSERT_TRUE(future.IsReady());

  int scheduled = 0;
  bool called = false;
  MemRpc::RpcThenExecutor executor = [&](std::function<void()> task) {
    ++scheduled;
    task();
  };

  future.Then([&](MemRpc::RpcReply) { called = true; }, executor);

  EXPECT_EQ(scheduled, 1);
  EXPECT_TRUE(called);
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R RpcClientApiTest`
Expected: build failure (no `RpcThenExecutor` / `Then` signature mismatch).

**Step 3: Write minimal implementation**

In `include/memrpc/client/rpc_client.h`:
- Add executor type alias:

```cpp
using RpcThenExecutor = std::function<void(std::function<void()>)>;
```

- Update `RpcFuture::Then` signature:

```cpp
void Then(std::function<void(RpcReply)> callback, RpcThenExecutor executor = {});
```

In `src/client/rpc_client.cpp`:
- Extend `RpcFuture::State`:

```cpp
std::function<void(RpcReply)> callback;
RpcThenExecutor executor;
```

- Update `RpcFuture::Then` to store executor and dispatch via executor if present.
- Update completion path (`ResolveFuture` and `CompleteRequest`) to:
  - Move out `callback + executor`.
  - Unlock.
  - If executor set, call `executor([cb = std::move(callback), reply = std::move(reply)]() mutable { cb(std::move(reply)); });`
  - Else call `callback(std::move(reply))` inline.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R RpcClientApiTest`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp
git commit -m "feat: add executor-aware Then"
```

---

### Task 2: Add `RpcSyncClient` and migrate sync call sites

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_client_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_dfx_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_async_pipeline_test.cpp`
- Modify: `include/apps/minirpc/parent/minirpc_client.h`
- Modify: `src/apps/minirpc/parent/minirpc_client.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`

**Step 1: Write a failing compilation change**

Update one integration test to use the new sync client (this will fail until `RpcSyncClient` exists):

```cpp
MemRpc::RpcSyncClient sync_client(bootstrap);
ASSERT_EQ(sync_client.Init(), MemRpc::StatusCode::Ok);
EXPECT_EQ(sync_client.InvokeSync(call, &reply), MemRpc::StatusCode::Ok);
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R RpcClientIntegrationTest`
Expected: compile failure (unknown `RpcSyncClient`).

**Step 3: Implement `RpcSyncClient` and move `InvokeSync` logic**

In `include/memrpc/client/rpc_client.h`:
- Remove `RpcClient::InvokeSync` declaration.
- Add `class RpcSyncClient` with:
  - `explicit RpcSyncClient(std::shared_ptr<IBootstrapChannel> bootstrap = nullptr);`
  - `StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);`
  - Forwarding methods: `Init`, `Shutdown`, `SetBootstrapChannel`, `SetEventCallback`, `SetFailureCallback`, `GetRuntimeStats`.
  - `private: RpcClient client_;`

In `src/client/rpc_client.cpp`:
- Delete `RpcClient::InvokeSync` implementation.
- Add `RpcSyncClient` methods and reuse existing `InvokeSync` logic using `client_.InvokeAsync(...)` + wait budget.

**Step 4: Migrate sync call sites**

- Replace `RpcClient` + `InvokeSync` usage in tests with `RpcSyncClient`.
- Update `MiniRpcClient` to wrap `RpcSyncClient` instead of `RpcClient`.
- Update `virus_engine_manager.cpp` to hold a `RpcSyncClient` (or adapt its existing client pointer type).

**Step 5: Run tests to verify they pass**

Run (subset):
- `ctest --test-dir build --output-on-failure -R RpcClientIntegrationTest`
- `ctest --test-dir build --output-on-failure -R ResponseQueueEventTest`
- `ctest --test-dir build --output-on-failure -R MiniRpcClientTest`

Expected: PASS

**Step 6: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp \
  tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp \
  tests/apps/minirpc/minirpc_client_test.cpp tests/apps/minirpc/minirpc_dfx_test.cpp \
  tests/apps/minirpc/minirpc_async_pipeline_test.cpp \
  include/apps/minirpc/parent/minirpc_client.h src/apps/minirpc/parent/minirpc_client.cpp \
  src/apps/vps/parent/virus_engine_manager.cpp
git commit -m "feat: add RpcSyncClient and migrate sync call sites"
```

---

### Task 3: Rename typed async helper to `Then<Rep>` + test

**Files:**
- Modify: `include/memrpc/client/typed_invoker.h`
- Modify: `tests/apps/minirpc/minirpc_client_test.cpp`

**Step 1: Write the failing test**

Add a typed-`Then` test after `SyncAndAsyncCallsRoundTrip`:

```cpp
TEST(MiniRpcClientTest, TypedThenDecodesReply) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MemRpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  EchoRequest req;
  req.text = "typed-then";

  std::atomic<bool> called = false;
  std::mutex mutex;
  EchoReply received;

  auto future = MemRpc::InvokeTyped(&client, MemRpc::Opcode::MiniEcho, req);
  MemRpc::Then<EchoReply>(std::move(future),
      [&](MemRpc::StatusCode status, EchoReply reply) {
        EXPECT_EQ(status, MemRpc::StatusCode::Ok);
        std::lock_guard<std::mutex> lock(mutex);
        received = std::move(reply);
        called = true;
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!called && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(called);
  EXPECT_EQ(received.text, "typed-then");

  client.Shutdown();
  server.Stop();
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R MiniRpcClientTest`
Expected: compile failure (missing `Then<Rep>` helper).

**Step 3: Implement minimal helper**

In `include/memrpc/client/typed_invoker.h`:
- Rename `ThenDecode` to:

```cpp
template <typename Rep>
void Then(RpcFuture future,
          std::function<void(StatusCode, Rep)> callback,
          RpcThenExecutor executor = {}) {
  future.Then([cb = std::move(callback)](RpcReply rpcReply) {
    if (rpcReply.status != StatusCode::Ok) {
      cb(rpcReply.status, {});
      return;
    }
    Rep decoded{};
    if (!DecodeMessage<Rep>(rpcReply.payload, &decoded)) {
      cb(StatusCode::ProtocolMismatch, {});
      return;
    }
    cb(rpcReply.status, std::move(decoded));
  }, std::move(executor));
}
```

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R MiniRpcClientTest`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/typed_invoker.h tests/apps/minirpc/minirpc_client_test.cpp
git commit -m "feat: add typed Then helper"
```

---

### Task 4: Full build + test sweep

**Files:**
- None (verification only)

**Step 1: Build**

Run: `cmake --build build`
Expected: build succeeds

**Step 2: Run full tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS

**Step 3: Commit**

No commit required (verification only).

---

**Note:** Implement this plan using @superpowers:executing-plans as required.
