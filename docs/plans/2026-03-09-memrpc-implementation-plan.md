# MemRPC Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a C++17 shared-memory RPC framework for a split antivirus engine with synchronous client compatibility, dual-priority queues, eventfd synchronization, Linux demo bootstrap, documentation, tests, and equivalent GN build files.

**Architecture:** The implementation is split into transport core, client runtime, server runtime, bootstrap abstraction, Linux demo bootstrap, and documentation/demo binaries. A fixed shared-memory layout with high/normal request rings, one response ring, and a slot pool carries scan requests and results, while a bootstrap interface isolates HarmonyOS SA integration from the IPC core.

**Tech Stack:** C++17, CMake, GN, POSIX shared memory, eventfd, threads, condition variables, GoogleTest or a lightweight CTest-based test binary.

---

### Task 1: Scaffold project structure and top-level build

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/memrpc/types.h`
- Create: `include/memrpc/client.h`
- Create: `include/memrpc/server.h`
- Create: `include/memrpc/bootstrap.h`
- Create: `include/memrpc/handler.h`
- Create: `src/.gitkeep`
- Create: `tests/.gitkeep`
- Create: `demo/.gitkeep`

**Step 1: Write the failing test**

Create a smoke test target declaration in `tests/CMakeLists.txt` that references a non-existent source file.

**Step 2: Run test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: FAIL because the referenced test source file does not exist yet.

**Step 3: Write minimal implementation**

Add the top-level CMake project, library targets, demo targets, and a real smoke test source file placeholder.

**Step 4: Run test to verify it passes**

Run: `cmake -S . -B build && cmake --build build`
Expected: PASS for configure/build.

### Task 2: Define public types and status model

**Files:**
- Modify: `include/memrpc/types.h`
- Test: `tests/types_test.cpp`

**Step 1: Write the failing test**

Add tests that assert:

- default `ScanOptions` uses normal priority
- `ScanResult` defaults to `kOk`/`kUnknown`
- status enum values needed by the design exist

**Step 2: Run test to verify it fails**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R types_test`
Expected: FAIL because the public types are not implemented.

**Step 3: Write minimal implementation**

Define the enums and structs in `include/memrpc/types.h`.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R types_test`
Expected: PASS.

### Task 3: Define bootstrap, handler, client, and server interfaces

**Files:**
- Modify: `include/memrpc/bootstrap.h`
- Modify: `include/memrpc/handler.h`
- Modify: `include/memrpc/client.h`
- Modify: `include/memrpc/server.h`
- Test: `tests/api_headers_test.cpp`

**Step 1: Write the failing test**

Add a compile-only test that instantiates interface references and verifies the headers compose cleanly.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R api_headers_test`
Expected: FAIL because the interfaces are incomplete.

**Step 3: Write minimal implementation**

Define the public API exactly as designed.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R api_headers_test`
Expected: PASS.

### Task 4: Implement ring buffer primitive

**Files:**
- Create: `src/core/ring_buffer.h`
- Create: `src/core/ring_buffer.cpp`
- Test: `tests/ring_buffer_test.cpp`

**Step 1: Write the failing test**

Add tests covering:

- push/pop one entry
- full ring rejection
- FIFO ordering

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R ring_buffer_test`
Expected: FAIL because the ring implementation does not exist.

**Step 3: Write minimal implementation**

Implement a bounded ring abstraction suitable for shared-memory-backed entries.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R ring_buffer_test`
Expected: PASS.

### Task 5: Implement slot pool lifecycle

**Files:**
- Create: `src/core/slot_pool.h`
- Create: `src/core/slot_pool.cpp`
- Test: `tests/slot_pool_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- reserve/free slot
- reserve until exhausted
- reject invalid state transitions

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R slot_pool_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement slot bookkeeping and state transitions.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R slot_pool_test`
Expected: PASS.

### Task 6: Define protocol and shared-memory layout helpers

**Files:**
- Create: `src/core/protocol.h`
- Create: `src/core/shm_layout.h`
- Create: `src/core/timeouts.h`
- Test: `tests/protocol_layout_test.cpp`

**Step 1: Write the failing test**

Add tests checking:

- header magic/version constants
- request/response entry sizes are fixed
- layout sizing routine computes offsets consistently

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R protocol_layout_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement protocol structs and layout calculation helpers.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R protocol_layout_test`
Expected: PASS.

### Task 7: Implement session and shared-memory mapping helpers

**Files:**
- Create: `src/core/session.h`
- Create: `src/core/session.cpp`
- Test: `tests/session_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- mapping a region from an fd
- rejecting protocol mismatch
- reading queue capacities from the header

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R session_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement session attach/detach and validation helpers.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R session_test`
Expected: PASS.

### Task 8: Implement pending request table and waiter context

**Files:**
- Create: `src/client/pending_map.h`
- Create: `src/client/pending_map.cpp`
- Test: `tests/pending_map_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- register/unregister request
- waiter wake-up by `request_id`
- duplicate request rejection

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R pending_map_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement the pending request map using mutex/condition-variable coordination.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R pending_map_test`
Expected: PASS.

### Task 9: Implement client result dispatcher

**Files:**
- Create: `src/client/response_dispatcher.h`
- Create: `src/client/response_dispatcher.cpp`
- Test: `tests/response_dispatcher_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- draining one response
- waking the correct pending waiter
- ignoring unknown request ids safely

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R response_dispatcher_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement the dispatcher that drains `resp_eventfd` and the response ring.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R response_dispatcher_test`
Expected: PASS.

### Task 10: Implement client synchronous compatibility layer

**Files:**
- Create: `src/client/engine_client.cpp`
- Test: `tests/engine_client_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- invalid request rejection
- enqueue high vs normal based on `ScanOptions`
- timeout translation to `StatusCode`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R engine_client_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement `EngineClient::Init`, `Scan`, and `Shutdown` using the session, slot pool, ring helpers, and pending map.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R engine_client_test`
Expected: PASS.

### Task 11: Implement server worker pools and dispatcher

**Files:**
- Create: `src/server/worker_pool.h`
- Create: `src/server/worker_pool.cpp`
- Create: `src/server/dispatcher.h`
- Create: `src/server/dispatcher.cpp`
- Test: `tests/server_dispatcher_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- high-priority work reaches the high-priority pool
- normal work reaches the normal pool
- a high-priority burst can starve normal work

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R server_dispatcher_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement the two worker pools and dispatcher behavior.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R server_dispatcher_test`
Expected: PASS.

### Task 12: Implement server runtime

**Files:**
- Create: `src/server/engine_server.cpp`
- Test: `tests/engine_server_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- handler invocation
- writing responses back into the response ring
- engine internal error mapping

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R engine_server_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement `EngineServer::Start`, `Run`, and `Stop`.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R engine_server_test`
Expected: PASS.

### Task 13: Implement reconnect-safe request outcome handling

**Files:**
- Modify: `src/client/engine_client.cpp`
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Test: `tests/recovery_policy_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- retry only before dispatch
- no retry after dispatch
- peer disconnect returns transport error for in-flight work

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R recovery_policy_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement the one-retry transport recovery policy and state checks.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R recovery_policy_test`
Expected: PASS.

### Task 14: Implement POSIX demo bootstrap

**Files:**
- Create: `src/bootstrap/posix_demo_bootstrap.h`
- Create: `src/bootstrap/posix_demo_bootstrap.cpp`
- Test: `tests/posix_demo_bootstrap_test.cpp`

**Step 1: Write the failing test**

Add tests for:

- creating bootstrap handles
- connecting client/server to the same session
- session id propagation

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R posix_demo_bootstrap_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Implement a Linux-only bootstrap that can create the shared memory object and eventfds for demos/tests.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R posix_demo_bootstrap_test`
Expected: PASS.

### Task 15: Add HarmonyOS SA bootstrap stub

**Files:**
- Create: `src/bootstrap/sa_bootstrap.h`
- Create: `src/bootstrap/sa_bootstrap.cpp`
- Test: `tests/sa_bootstrap_stub_test.cpp`

**Step 1: Write the failing test**

Add a compile-only test that checks the stub implements `IBootstrapChannel` and exposes placeholder extension points.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R sa_bootstrap_stub_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Add the stub adapter surface and explicit TODO-style placeholder behavior without hard dependency on HarmonyOS SDKs.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R sa_bootstrap_stub_test`
Expected: PASS.

### Task 16: Build end-to-end integration tests

**Files:**
- Create: `tests/integration_end_to_end_test.cpp`

**Step 1: Write the failing test**

Add integration tests covering:

- single request round-trip
- concurrent requests
- high-priority request completing ahead of queued normal requests
- queue timeout
- exec timeout

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R integration_end_to_end_test`
Expected: FAIL.

**Step 3: Write minimal implementation**

Fill missing glue code and synchronization needed to satisfy the end-to-end behavior.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R integration_end_to_end_test`
Expected: PASS.

### Task 17: Add demo binaries

**Files:**
- Create: `demo/demo_scan_handler.cpp`
- Create: `demo/demo_engine_main.cpp`
- Create: `demo/demo_client_main.cpp`

**Step 1: Write the failing test**

Add a build-only demo target check by referencing the demo sources before they exist.

**Step 2: Run test to verify it fails**

Run: `cmake --build build`
Expected: FAIL because the demo source files do not exist yet.

**Step 3: Write minimal implementation**

Add the demo binaries and fake handler.

**Step 4: Run test to verify it passes**

Run: `cmake --build build`
Expected: PASS.

### Task 18: Write user-facing documentation

**Files:**
- Create: `docs/architecture.md`
- Create: `docs/demo_guide.md`
- Create: `docs/porting_guide.md`
- Create: `docs/sa_integration.md`

**Step 1: Write the failing test**

Add a docs presence test script or CTest check that expects these files to exist.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R docs_presence_test`
Expected: FAIL because the docs are missing.

**Step 3: Write minimal implementation**

Write concise but complete documentation for architecture, demo usage, migration, and SA integration.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R docs_presence_test`
Expected: PASS.

### Task 19: Add GN build translation

**Files:**
- Create: `BUILD.gn`
- Create: `memrpc.gni`

**Step 1: Write the failing test**

Add a lightweight validation test that checks these files exist and mention all required targets.

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R gn_files_test`
Expected: FAIL because the GN files do not exist yet.

**Step 3: Write minimal implementation**

Translate the CMake targets and source lists into GN.

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R gn_files_test`
Expected: PASS.

### Task 20: Run final verification

**Files:**
- Verify only

**Step 1: Run targeted tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS.

**Step 2: Run demo build**

Run: `cmake --build build`
Expected: PASS.

**Step 3: Run demo manually**

Run: `./build/demo/demo_engine_main` in one terminal and `./build/demo/demo_client_main` in another
Expected: observable normal and high-priority requests, plus timeout/failure demo output.

**Step 4: Git status or workspace status**

Run: `git status --short` if inside a git repo; otherwise record that the workspace is not a git repository.
Expected: changed files listed or a clear non-git note.
