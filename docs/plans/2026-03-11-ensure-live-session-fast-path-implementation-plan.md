# EnsureLiveSession Fast-Path Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an atomic fast-path to `EnsureLiveSession()` to avoid lock contention when the session is already live.

**Architecture:** Introduce a `session_live` atomic flag that reflects “session is valid and slot_pool is ready.” The hot path reads this flag and returns without locking; reconnection and teardown update it under `session_mutex` with acquire/release ordering.

**Tech Stack:** C++17, std::atomic, std::mutex, GoogleTest via CMake.

---

## TODO
- Task execution cancelled on 2026-03-11; do not proceed without explicit re-approval.
- If resumed, reconcile workflow constraints (worktree + TDD) with the current repo state and other AI changes.

### Task 1: Add `session_live` atomic and hot-path check

**Files:**
- Modify: `src/client/rpc_client.cpp`

**Step 1: Add the atomic member**

Add the field near `session_dead`:

```cpp
std::atomic<bool> session_live{false};
```

**Step 2: Add the hot-path check to `EnsureLiveSession()`**

Insert at the very top of `EnsureLiveSession()`:

```cpp
if (session_live.load(std::memory_order_acquire)) {
  return StatusCode::Ok;
}
```

**Step 3: Double-check inside the slow path**

Inside the `reconnect_mutex` + `session_mutex` guarded block, replace the existing fast check with:

```cpp
if (session_live.load(std::memory_order_relaxed)) {
  return StatusCode::Ok;
}
```

**Step 4: Keep `session_live` false during teardown in reconnection**

Before clearing `session`/`slot_pool` in the reconnection flow, set:

```cpp
session_live.store(false, std::memory_order_release);
```

**Step 5: Mark live after successful attach**

After `slot_pool` is created and `session_dead` is set false, add:

```cpp
session_live.store(true, std::memory_order_release);
```

**Step 6: Run format/build check (no tests yet)**

Run: `cmake -S . -B build`
Expected: configure succeeds

Run: `cmake --build build`
Expected: build succeeds

**Step 7: Commit**

```bash
git add src/client/rpc_client.cpp
git commit -m "fix: add EnsureLiveSession fast-path"
```

---

### Task 2: Clear `session_live` on engine death and shutdown

**Files:**
- Modify: `src/client/rpc_client.cpp`

**Step 1: Update `HandleEngineDeath()` teardown path**

Inside the `session_mutex` block, before resetting `session`/`slot_pool`, add:

```cpp
session_live.store(false, std::memory_order_release);
```

**Step 2: Update `Shutdown()` teardown path**

Inside the `session_mutex` block, before resetting `session`/`slot_pool`, add:

```cpp
impl_->session_live.store(false, std::memory_order_release);
```

**Step 3: Run targeted tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass

**Step 4: Commit**

```bash
git add src/client/rpc_client.cpp
git commit -m "fix: clear session_live on teardown"
```

---

### Task 3: (Optional) Add a focused unit test

**Files:**
- Modify: `tests/...` (only if feasible without exposing private internals)

**Step 1: Decide if test is feasible without widening API**

If it requires new public surface or test-only hooks, skip this task to keep scope small.

**Step 2: If feasible, write a minimal test**

Assert that a live session returns `StatusCode::Ok` without calling `OpenSession()`.

**Step 3: Run the specific test**

Run: `ctest --test-dir build -R rpc_client -V`
Expected: targeted test passes

**Step 4: Commit**

```bash
git add tests/...
git commit -m "test: add EnsureLiveSession fast-path coverage"
```
