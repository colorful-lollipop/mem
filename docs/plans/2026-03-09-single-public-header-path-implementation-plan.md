# 单一公开头路径 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 删除 `memrpc` 顶层转发头，只保留 `core/client/server` 三层公开头路径。

**Architecture:** 先用测试钉住“主线只允许分层头路径”的边界，再批量替换主线源码和测试中的 include，最后删除顶层转发头并更新文档。`legacy/` 保持不动。

**Tech Stack:** C++17、CMake、GTest、Git

---

### Task 1: 为单一公开头路径补失败测试

**Files:**
- Modify: `tests/memrpc/framework_split_headers_test.cpp`
- Modify: `tests/memrpc/build_config_test.cpp`

**Step 1: Write the failing test**

补断言：

- 主线测试和 demo 不再依赖 `memrpc/rpc_client.h`、`memrpc/rpc_server.h` 等顶层头
- `include/memrpc/` 顶层只允许目录型入口，不再允许这些转发头长期存在

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_framework_split_headers_test|memrpc_build_config_test'`

Expected:

- FAIL，直到主线 include 已全部切到分层路径

**Step 3: Write minimal implementation**

只改测试，不改生产代码。

**Step 4: Run test to verify it still fails for the right reason**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_framework_split_headers_test|memrpc_build_config_test'`

Expected:

- FAIL，且失败集中在主线仍引用顶层头

**Step 5: Commit**

```bash
git add tests/memrpc/framework_split_headers_test.cpp tests/memrpc/build_config_test.cpp
git commit -m "test: cover single public header path"
```

### Task 2: 切换主线源码和测试到分层头路径

**Files:**
- Modify: `src/core/session.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/bootstrap/sa_bootstrap.cpp`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `demo/minirpc_demo_main.cpp`
- Modify: `tests/memrpc/*.cpp`
- Modify: `tests/apps/minirpc/*.cpp`

**Step 1: Write the failing test**

依赖 Task 1 的 include 边界测试。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_framework_split_headers_test|memrpc_build_config_test'`

Expected:

- FAIL

**Step 3: Write minimal implementation**

只替换主线中的 include：

- `memrpc/core/*`
- `memrpc/client/*`
- `memrpc/server/*`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_framework_split_headers_test|memrpc_build_config_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src demo tests apps/minirpc
git commit -m "refactor: use layered memrpc public headers"
```

### Task 3: 删除顶层转发头

**Files:**
- Delete: `include/memrpc/bootstrap.h`
- Delete: `include/memrpc/types.h`
- Delete: `include/memrpc/client.h`
- Delete: `include/memrpc/demo_bootstrap.h`
- Delete: `include/memrpc/sa_bootstrap.h`
- Delete: `include/memrpc/handler.h`
- Delete: `include/memrpc/rpc_client.h`
- Delete: `include/memrpc/rpc_server.h`
- Delete: `include/memrpc/server.h`

**Step 1: Write the failing test**

依赖 build config / framework header 边界测试。

**Step 2: Run test to verify it fails**

Run: `cmake --build build`

Expected:

- FAIL，直到主线已完全不依赖顶层头

**Step 3: Write minimal implementation**

删除上述顶层转发头。

**Step 4: Run test to verify it passes**

Run: `cmake --build build`

Expected:

- BUILD PASS

**Step 5: Commit**

```bash
git add include/memrpc
git commit -m "refactor: remove top-level memrpc forwarding headers"
```

### Task 4: 更新文档并全量回归

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/architecture.md`
- Modify: `docs/porting_guide.md`

**Step 1: Write the failing test**

确保文档声明与当前边界一致：

- 只保留一套公开头路径
- 新代码必须使用分层路径

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- 如文档和边界不一致则 FAIL

**Step 3: Write minimal implementation**

补文档中的 include 规范。

**Step 4: Run full verification**

Run: `cmake --build build`

Expected:

- BUILD PASS

Run: `ctest --test-dir build --output-on-failure`

Expected:

- 全部 PASS

Run: `./build/demo/memrpc_minirpc_demo`

Expected:

- 正常输出基础 RPC 结果

**Step 5: Commit**

```bash
git add AGENTS.md docs/architecture.md docs/porting_guide.md
git commit -m "docs: require layered memrpc header paths"
```
