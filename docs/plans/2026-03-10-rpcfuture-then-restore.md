# RpcFuture Then Restoration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Restore the original `RpcFuture::Then` callback logic so `Then` links and behaves as previously implemented.

**Architecture:** Add a callback field in `RpcFuture::State`, implement `Then()` to register or immediately invoke the callback, and route completion paths (`ResolveFuture`, `CompleteRequest`) to invoke the callback when present. No exclusivity enforcement is added; behavior matches the earlier implementation.

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: Capture The Current Failure

**Files:**
- Test: `tests/memrpc/rpc_client_api_test.cpp`

**Step 1: Run the failing build**

Run: `cmake --build build --target memrpc_rpc_client_api_test`
Expected: FAIL with undefined reference to `memrpc::RpcFuture::Then(std::function<void (memrpc::RpcReply)>)`.

**Step 2: Commit (optional, if you need a checkpoint)**

Skip commit for this task unless you need a baseline checkpoint.

---

### Task 2: Restore Then Callback Logic

**Files:**
- Modify: `src/client/rpc_client.cpp`

**Step 1: Add callback to `RpcFuture::State` and implement `Then()`**

```cpp
struct RpcFuture::State {
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool abandoned = false;
  RpcReply reply;
  std::function<void(RpcReply)> callback;
};

void RpcFuture::Then(std::function<void(RpcReply)> callback) {
  if (state_ == nullptr || !callback) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->ready) {
    RpcReply reply = std::move(state_->reply);
    lock.unlock();
    callback(std::move(reply));
    return;
  }
  state_->callback = std::move(callback);
}
```

**Step 2: Route completion paths to callback when present**

```cpp
void ResolveFuture(const std::shared_ptr<RpcFuture::State>& pending, StatusCode status) {
  ...
  std::unique_lock<std::mutex> lock(pending->mutex);
  if (pending->ready) {
    return;
  }
  pending->reply.status = status;
  pending->ready = true;
  if (pending->callback) {
    auto cb = std::move(pending->callback);
    RpcReply reply = std::move(pending->reply);
    lock.unlock();
    cb(std::move(reply));
  } else {
    pending->cv.notify_one();
  }
}

// In CompleteRequest() when pending != nullptr
std::unique_lock<std::mutex> lock(pending->mutex);
if (!pending->abandoned) {
  pending->reply = std::move(reply);
  pending->ready = true;
  if (pending->callback) {
    auto cb = std::move(pending->callback);
    RpcReply cb_reply = std::move(pending->reply);
    lock.unlock();
    cb(std::move(cb_reply));
  } else {
    pending->cv.notify_one();
  }
}
```

**Step 3: Build to confirm fix**

Run: `cmake --build build --target memrpc_rpc_client_api_test`
Expected: PASS.

**Step 4: Run focused tests**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_api_test`
Expected: PASS.

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp
git commit -m "fix: restore RpcFuture Then callback logic"
```

---

Plan complete and saved to `docs/plans/2026-03-10-rpcfuture-then-restore.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
