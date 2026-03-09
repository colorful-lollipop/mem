# Session Recovery Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add death-callback-driven session invalidation and lazy automatic reconnect to `memrpc`, plus compile database export and documentation updates.

**Architecture:** The bootstrap layer reports engine death to the client. The client invalidates the active session immediately, fails old pending work, and lazily recreates a fresh shared-memory session on the next `Scan()`. Only requests that were never published to the old ring are retried once.

**Tech Stack:** C++17, CMake, GN, POSIX shared memory, eventfd, GoogleTest.

---

### Task 1: Export compile_commands and document the new recovery model

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`

**Step 1: Write the failing test**

Add a small test or configure assertion that expects `CMAKE_EXPORT_COMPILE_COMMANDS` to be enabled in the top-level CMake file.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_build_config_test`
Expected: FAIL because compile database export is not enforced yet.

**Step 3: Write minimal implementation**

Enable compile database export and update docs to describe death-callback-driven recovery.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_build_config_test`
Expected: PASS.

### Task 2: Extend bootstrap interfaces for death callbacks

**Files:**
- Modify: `include/memrpc/bootstrap.h`
- Modify: `include/memrpc/demo_bootstrap.h`
- Modify: `include/memrpc/sa_bootstrap.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `src/bootstrap/sa_bootstrap.cpp`
- Test: `tests/bootstrap_callback_test.cpp`

**Step 1: Write the failing test**

Add tests that require:

- bootstrap can register a death callback
- fake-SA bootstrap can trigger that callback for tests
- restarting produces a newer `session_id`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_bootstrap_callback_test`
Expected: FAIL because the callback API does not exist.

**Step 3: Write minimal implementation**

Add callback registration and a test hook that simulates engine death and session replacement.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_bootstrap_callback_test`
Expected: PASS.

### Task 3: Add client recovery tests

**Files:**
- Modify: `tests/integration_end_to_end_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- pending request is failed immediately by death callback
- next `Scan()` recovers onto a new session
- unpublished request is retried once
- published request is not retried

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`
Expected: FAIL because session recovery is not implemented.

**Step 3: Write minimal implementation**

Do not implement yet. Stop after confirming the tests fail for the expected reasons.

### Task 4: Implement EngineClient session invalidation and lazy reconnect

**Files:**
- Modify: `include/memrpc/client.h`
- Modify: `src/client/engine_client.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`

**Step 1: Write minimal implementation**

Implement:

- death callback registration in `Init()`
- active session invalidation
- pending request wake-up on death
- ensure-live-session helper
- one-time retry only before ring publication

**Step 2: Run tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(bootstrap_callback|integration_end_to_end)_test'`
Expected: PASS.

### Task 5: Verify the full project and update GN parity

**Files:**
- Modify: `BUILD.gn`
- Modify: `memrpc.gni`

**Step 1: Verify GN translation still matches the CMake surface**

Add any new headers/tests introduced by recovery work.

**Step 2: Run full verification**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure && ./build/demo/memrpc_demo_dual_process`
Expected: PASS.

**Step 3: Commit**

```bash
git add CMakeLists.txt BUILD.gn memrpc.gni include src tests docs
git commit -m "feat: add death-driven session recovery"
```
