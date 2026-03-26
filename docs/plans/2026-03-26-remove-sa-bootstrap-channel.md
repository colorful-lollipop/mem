# Remove `SaBootstrapChannel` Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove the fake `SaBootstrapChannel` shim from `memrpc`, replace its remaining test usage with `DevBootstrapChannel`, and leave the repo with a clearer bootstrap split: framework tests use `DevBootstrapChannel`, VES runtime uses `VesBootstrapChannel`.

**Architecture:** `SaBootstrapChannel` is currently just a thin wrapper around `DevBootstrapChannel`, so it adds naming confusion without adding a distinct runtime path. The change should delete that wrapper completely, retarget the few `memrpc` tests that still instantiate it, and remove build/test registrations for the deleted code. No `RpcClient` behavior change is intended in this task.

**Tech Stack:** C++17, CMake, GN, GoogleTest, shared-memory RPC bootstrap/session code.

---

### Task 1: Replace `SaBootstrapChannel` Usage In Existing Tests

**Files:**
- Modify: `/root/code/demo/mem/memrpc/tests/rpc_server_executor_test.cpp`
- Modify: `/root/code/demo/mem/memrpc/tests/typed_future_test.cpp`
- Modify: `/root/code/demo/mem/memrpc/tests/bootstrap_health_check_test.cpp`

**Step 1: Write the failing test/compile change**

Change the includes and constructions from:

```cpp
#include "memrpc/client/sa_bootstrap.h"
auto bootstrap = std::make_shared<MemRpc::SaBootstrapChannel>();
server.SetBootstrapHandles(bootstrap->ServerHandles());
```

to:

```cpp
#include "memrpc/client/dev_bootstrap.h"
auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
server.SetBootstrapHandles(bootstrap->serverHandles());
```

Also replace the health-check assertion that currently instantiates `SaBootstrapChannel` with a `DevBootstrapChannel` or remove the duplicate coverage if it becomes redundant.

**Step 2: Run tests to verify the edited files compile and pass**

Run:

```bash
ctest --test-dir /root/code/demo/mem/build_ninja --output-on-failure -R "memrpc_(rpc_server_executor|typed_future|bootstrap_health_check)_test"
```

Expected: PASS. No remaining compile dependency on `memrpc/client/sa_bootstrap.h` in these tests.

**Step 3: Commit**

```bash
git -C /root/code/demo/mem add memrpc/tests/rpc_server_executor_test.cpp memrpc/tests/typed_future_test.cpp memrpc/tests/bootstrap_health_check_test.cpp
git -C /root/code/demo/mem commit -m "refactor: stop using sa bootstrap in memrpc tests"
```

### Task 2: Remove The Dedicated `SaBootstrapChannel` Stub Test

**Files:**
- Delete: `/root/code/demo/mem/memrpc/tests/sa_bootstrap_stub_test.cpp`
- Modify: `/root/code/demo/mem/memrpc/tests/CMakeLists.txt`
- Modify: `/root/code/demo/mem/memrpc/BUILD.gn`

**Step 1: Remove the obsolete test**

Delete the stub-specific test file entirely. Its assertions only verify behavior inherited from `DevBootstrapChannel`, so it does not protect unique functionality anymore.

**Step 2: Remove test registration**

Delete the related registrations:

```cmake
memrpc_add_test(memrpc_sa_bootstrap_stub_test sa_bootstrap_stub_test.cpp)
```

and the corresponding GN test target:

```gn
repo_gtest("memrpc_sa_bootstrap_stub_test") {
  sources = [ "tests/sa_bootstrap_stub_test.cpp" ]
}
```

**Step 3: Run tests to verify no stale registration remains**

Run:

```bash
ctest --test-dir /root/code/demo/mem/build_ninja --output-on-failure -R "memrpc_(rpc_server_executor|typed_future|bootstrap_health_check)_test"
```

Expected: PASS. No CTest target named `memrpc_sa_bootstrap_stub_test` remains in regenerated build files after reconfigure/build.

**Step 4: Commit**

```bash
git -C /root/code/demo/mem add memrpc/tests/CMakeLists.txt memrpc/BUILD.gn
git -C /root/code/demo/mem rm memrpc/tests/sa_bootstrap_stub_test.cpp
git -C /root/code/demo/mem commit -m "test: remove obsolete sa bootstrap stub coverage"
```

### Task 3: Delete `SaBootstrapChannel` Source And Build References

**Files:**
- Delete: `/root/code/demo/mem/memrpc/include/memrpc/client/sa_bootstrap.h`
- Delete: `/root/code/demo/mem/memrpc/src/bootstrap/sa_bootstrap.cpp`
- Modify: `/root/code/demo/mem/memrpc/src/CMakeLists.txt`
- Modify: `/root/code/demo/mem/memrpc/BUILD.gn`
- Modify: `/root/code/demo/mem/virus_executor_service/CMakeLists.txt`

**Step 1: Remove source/header from the tree**

Delete:

```text
memrpc/include/memrpc/client/sa_bootstrap.h
memrpc/src/bootstrap/sa_bootstrap.cpp
```

**Step 2: Remove build references**

Delete the `sa_bootstrap.cpp` entry from:

- `memrpc/src/CMakeLists.txt`
- `memrpc/BUILD.gn`
- `virus_executor_service/CMakeLists.txt`

The `virus_executor_service` CMake file currently pulls that memrpc source in transitively; this must be cleaned up or the build will reference a deleted file.

**Step 3: Rebuild to verify removal is complete**

Run:

```bash
cmake --build /root/code/demo/mem/build_ninja --parallel
```

Expected: PASS. No include or source-file reference to `sa_bootstrap` remains.

**Step 4: Commit**

```bash
git -C /root/code/demo/mem add memrpc/src/CMakeLists.txt memrpc/BUILD.gn virus_executor_service/CMakeLists.txt
git -C /root/code/demo/mem rm memrpc/include/memrpc/client/sa_bootstrap.h memrpc/src/bootstrap/sa_bootstrap.cpp
git -C /root/code/demo/mem commit -m "refactor: remove fake sa bootstrap channel"
```

### Task 4: Clean Up Remaining Mentions And Architecture Notes

**Files:**
- Modify: `/root/code/demo/mem/CLAUDE.md`
- Search/Modify as needed: `/root/code/demo/mem/docs/`

**Step 1: Update architecture wording**

Replace statements that present `SaBootstrapChannel` as a real implementation path with wording that matches the new split:

- `DevBootstrapChannel` for framework/dev/test bootstrap
- `VesBootstrapChannel` for VES runtime/service control

If a document is only speculative and clearly marked as a future Harmony plan, leave it alone unless it becomes misleading after deletion.

**Step 2: Verify no stale source references remain**

Run:

```bash
rg -n "SaBootstrapChannel|sa_bootstrap" /root/code/demo/mem -g '!build*' -g '!docs/plans/**'
```

Expected: only intentional historical/planning mentions remain, or no matches at all outside archived plan docs.

**Step 3: Commit**

```bash
git -C /root/code/demo/mem add CLAUDE.md docs
git -C /root/code/demo/mem commit -m "docs: remove sa bootstrap references"
```

### Task 5: Final Verification

**Files:**
- Verify: `/root/code/demo/mem/memrpc/tests/`
- Verify: `/root/code/demo/mem/virus_executor_service/tests/`

**Step 1: Run focused bootstrap/client/server test subset**

Run:

```bash
ctest --test-dir /root/code/demo/mem/build_ninja --output-on-failure -R "memrpc_(session|rpc_server_executor|typed_future|bootstrap_health_check|rpc_client_api)_test"
```

Expected: PASS.

**Step 2: Run VES-side bootstrap-sensitive subset**

Run:

```bash
ctest --test-dir /root/code/demo/mem/build_ninja --output-on-failure -R "virus_executor_service_(ves_policy|ves_heartbeat|testkit_client)_test"
```

Expected: PASS. This confirms the repo still has a valid runtime bootstrap story after removing the fake SA layer.

**Step 3: Optional stronger gate for confidence**

Run:

```bash
tools/build_and_test.sh --test-regex "memrpc_(session|rpc_server_executor|typed_future|bootstrap_health_check)|virus_executor_service_(ves_policy|ves_heartbeat|testkit_client)"
```

Expected: PASS.

**Step 4: Final commit**

```bash
git -C /root/code/demo/mem status --short
```

Expected: clean working tree except for any intentionally preserved plan files.
