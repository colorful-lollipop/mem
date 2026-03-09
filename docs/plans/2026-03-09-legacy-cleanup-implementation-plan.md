# 主线与遗留代码隔离 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `vps_demo`、旧 compat 层和非主线测试迁入 `legacy/`，只保留 `memrpc + minirpc` 作为主构建。

**Architecture:** 主线代码不重写功能，只做物理归档和构建隔离。先用测试和构建配置钉住“主线不再引用 legacy”，再迁移文件，最后补归档说明并全量回归。

**Tech Stack:** C++17、CMake、GTest、Git

---

### Task 1: 为 legacy 隔离补失败测试

**Files:**
- Modify: `tests/memrpc/build_config_test.cpp`
- Modify: `tests/memrpc/framework_split_headers_test.cpp`

**Step 1: Write the failing test**

补断言：

- 主构建只引用 `memrpc` 和 `apps/minirpc`
- `legacy/` 目录存在时，`src/CMakeLists.txt`、`tests/CMakeLists.txt`、`demo/CMakeLists.txt` 不应引用其中内容

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- FAIL，直到 legacy 目录与主构建边界一致

**Step 3: Write minimal implementation**

只更新测试，不改生产代码。

**Step 4: Run test to verify it still fails for the right reason**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- FAIL，且失败集中在 legacy 隔离尚未完成

**Step 5: Commit**

```bash
git add tests/memrpc/build_config_test.cpp tests/memrpc/framework_split_headers_test.cpp
git commit -m "test: cover legacy isolation boundaries"
```

### Task 2: 迁移 `vps_demo` 到 `legacy/`

**Files:**
- Create: `legacy/vps_demo/include/`
- Create: `legacy/vps_demo/src/`
- Move: `include/vps_demo/*`
- Move: `src/vps_demo/*`
- Create: `legacy/README.md`

**Step 1: Write the failing test**

依赖 Task 1 中的边界测试。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- FAIL

**Step 3: Write minimal implementation**

将 `vps_demo` 物理迁入 `legacy/vps_demo`，并在 `legacy/README.md` 标明“不纳入主构建，仅作历史参考”。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add legacy/README.md legacy/vps_demo include/vps_demo src/vps_demo
git commit -m "refactor: archive vps demo sources"
```

### Task 3: 迁移旧 compat 与 codec 残留到 `legacy/`

**Files:**
- Create: `legacy/memrpc_compat/include/`
- Create: `legacy/memrpc_compat/src/`
- Move: `include/memrpc/compat/*`
- Move: `src/client/engine_client.cpp`
- Move: `src/server/engine_server.cpp`
- Move: `src/rpc/*`
- Move: `src/memrpc/compat/*`

**Step 1: Write the failing test**

依赖 build config 断言：

- 主线源码目录不再存在旧 compat 入口

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R '^memrpc_build_config_test$'`

Expected:

- FAIL

**Step 3: Write minimal implementation**

将上述旧 compat 文件整体迁入 `legacy/memrpc_compat`。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R '^memrpc_build_config_test$'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add legacy/memrpc_compat include/memrpc/compat src/client/engine_client.cpp src/server/engine_server.cpp src/rpc src/memrpc/compat
git commit -m "refactor: archive legacy memrpc compat layer"
```

### Task 4: 迁移非主线测试到 `legacy/`

**Files:**
- Create: `legacy/tests/`
- Move: `tests/integration_end_to_end_test.cpp`
- Move: `tests/scan_codec_test.cpp`
- Move: `tests/scan_behavior_codec_test.cpp`
- Move: `tests/vps_codec_test.cpp`
- Move: `tests/vps_manager_integration_test.cpp`
- Move: `tests/vps_service_test.cpp`

**Step 1: Write the failing test**

依赖 build config 断言：

- 主线 `tests/` 只保留 `tests/memrpc` 与 `tests/apps/minirpc`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R '^memrpc_build_config_test$'`

Expected:

- FAIL

**Step 3: Write minimal implementation**

迁移非主线测试到 `legacy/tests/`。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R '^memrpc_build_config_test$'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add legacy/tests tests/integration_end_to_end_test.cpp tests/scan_codec_test.cpp tests/scan_behavior_codec_test.cpp tests/vps_codec_test.cpp tests/vps_manager_integration_test.cpp tests/vps_service_test.cpp
git commit -m "refactor: archive non-mainline tests"
```

### Task 5: 全量回归主线

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/porting_guide.md`

**Step 1: Write the failing test**

确认文档和主线描述一致：

- 主线只关注 `memrpc + minirpc`
- `legacy/` 目录只作参考

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R '^memrpc_build_config_test$'`

Expected:

- 如文档未同步则 FAIL

**Step 3: Write minimal implementation**

同步文档中的主线与 legacy 说明。

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
git add docs/architecture.md docs/porting_guide.md legacy
git commit -m "docs: describe mainline and legacy split"
```
