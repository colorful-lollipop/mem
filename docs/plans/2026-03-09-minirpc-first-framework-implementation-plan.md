# MiniRpc 优先的框架通用化 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 先把 `MemRpc` 收敛成纯框架，再实现最小 `MiniRpc` 应用验证跨进程 RPC 和恢复能力，同时把所有业务兼容层移出主构建。

**Architecture:** 先统一命名空间和目录边界，再从 `memrpc` 主构建中移除业务兼容层、业务 codec 和 `vps_demo`，随后以 `apps/minirpc` 作为唯一应用样板验证同步包装、异步调用、优先级、超时和恢复。当前不引入任何代码生成系统，codec 和 glue 以手写为主，只提取少量公共 helper。监听型能力后续通过普通 `Poll...()` RPC 实现，不引入独立事件通道。

**Tech Stack:** C++17、shared memory、eventfd、GTest、CMake

---

### Task 1: 为新的命名空间和目录边界补护栏测试

**Files:**
- Modify: `tests/api_headers_test.cpp`
- Modify: `tests/framework_split_headers_test.cpp`
- Create: `tests/minirpc_headers_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

新增头文件护栏测试，要求：

- `MemRpc` 类型从 `OHOS::Security::VirusProtectionService::MemRpc` 可见
- `MiniRpc` 头从 `include/apps/minirpc/...` 可见
- 测试不依赖 `vps_demo` 目录

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_headers_test`
Expected:

- FAIL，`MiniRpc` 头和命名空间尚未建立

**Step 3: Write minimal implementation**

补最小头文件和转发结构，使测试先能编译通过。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_headers_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/api_headers_test.cpp tests/framework_split_headers_test.cpp tests/minirpc_headers_test.cpp tests/CMakeLists.txt
git commit -m "test: add minirpc header guards"
```

### Task 2: 统一框架命名空间到 `OHOS::Security::VirusProtectionService::MemRpc`

**Files:**
- Modify: `include/memrpc/**/*.h`
- Modify: `src/**/*.cpp`
- Modify: `tests/**/*.cpp`
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`

**Step 1: Write the failing test**

依赖现有公共 API 测试：

- `memrpc_api_headers_test`
- `memrpc_rpc_client_api_test`
- `memrpc_rpc_server_api_test`

补充命名空间断言，要求旧 `memrpc::` 直接引用不再是主路径。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_api_headers_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`
Expected:

- FAIL，命名空间尚未迁移完全

**Step 3: Write minimal implementation**

统一框架公共层和兼容层命名空间到：

- `OHOS::Security::VirusProtectionService::MemRpc`

必要时保留少量兼容 `using` 或别名，避免一次性打断所有引用。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_api_headers_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`
Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc src tests docs/architecture.md docs/demo_guide.md docs/porting_guide.md
git commit -m "refactor: move memrpc into vps namespace"
```

### Task 3: 清理框架侧业务污染并收缩主构建

**Files:**
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `demo/CMakeLists.txt`
- Modify: `tests/api_headers_test.cpp`
- Modify: `tests/framework_split_headers_test.cpp`
- Modify: `tests/build_config_test.cpp`

**Step 1: Write the failing test**

补护栏，要求主构建中不再出现：

- `engine_client.cpp`
- `engine_server.cpp`
- `scan_codec.cpp`
- `scan_behavior_codec.cpp`
- `vps_demo`
- `apps/vps`
- 旧双进程 demo

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_build_config_test`
Expected:

- FAIL，主构建尚未收缩

**Step 3: Write minimal implementation**

从主构建中移除：

- `engine_client.cpp`
- `engine_server.cpp`
- `src/rpc/*`
- `src/vps_demo/*`
- VPS 相关测试
- 旧双进程 demo

并把头文件测试同步到纯 `MemRpc + MiniRpc` 边界。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_build_config_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add src/CMakeLists.txt tests/CMakeLists.txt demo/CMakeLists.txt tests/api_headers_test.cpp tests/framework_split_headers_test.cpp tests/build_config_test.cpp
git commit -m "refactor: remove app compatibility from memrpc build"
```

### Task 4: 建立 `apps/minirpc` 最小公共类型和 codec

**Files:**
- Create: `include/apps/minirpc/common/minirpc_types.h`
- Create: `include/apps/minirpc/common/minirpc_codec.h`
- Create: `src/apps/minirpc/common/minirpc_codec.cpp`
- Create: `tests/minirpc_codec_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

**Step 1: Write the failing test**

覆盖以下 round-trip：

- `EchoRequest/Reply`
- `AddRequest/Reply`
- `SleepRequest/Reply`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_codec_test`
Expected:

- FAIL，类型和 codec 尚不存在

**Step 3: Write minimal implementation**

手写最小 codec：

- 只使用现有 `ByteWriter/ByteReader`
- 保持结构简单

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_codec_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_types.h include/apps/minirpc/common/minirpc_codec.h src/apps/minirpc/common/minirpc_codec.cpp tests/minirpc_codec_test.cpp tests/CMakeLists.txt src/CMakeLists.txt
git commit -m "feat: add minirpc common codecs"
```

### Task 5: 实现 `MiniRpc` 子进程 service

**Files:**
- Create: `include/apps/minirpc/child/minirpc_service.h`
- Create: `src/apps/minirpc/child/minirpc_service.cpp`
- Create: `tests/minirpc_service_test.cpp`
- Modify: `src/core/protocol.h`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

覆盖：

- `Echo`
- `Add`
- `Sleep`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_service_test`
Expected:

- FAIL，service 和 opcode 尚不存在

**Step 3: Write minimal implementation**

新增 `MiniRpc` opcode 和 `MiniRpcService`：

- 注册 handler
- 实现最小逻辑

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_service_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/child/minirpc_service.h src/apps/minirpc/child/minirpc_service.cpp src/core/protocol.h src/CMakeLists.txt tests/minirpc_service_test.cpp tests/CMakeLists.txt
git commit -m "feat: add minirpc child service"
```

### Task 6: 实现 `MiniRpc` 父进程异步客户端和同步包装

**Files:**
- Create: `include/apps/minirpc/parent/minirpc_async_client.h`
- Create: `include/apps/minirpc/parent/minirpc_client.h`
- Create: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Create: `src/apps/minirpc/parent/minirpc_client.cpp`
- Create: `tests/minirpc_client_test.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

覆盖：

- `InvokeAsync(Echo)`
- `InvokeSync(Add)`
- `Sleep` 超时返回
- 高优请求优先于普通请求完成

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_client_test`
Expected:

- FAIL，客户端尚不存在

**Step 3: Write minimal implementation**

实现：

- `MiniRpcAsyncClient`
- `MiniRpcClient`

只保留薄层包装，不重复 session / queue 逻辑。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_client_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/parent/minirpc_async_client.h include/apps/minirpc/parent/minirpc_client.h src/apps/minirpc/parent/minirpc_async_client.cpp src/apps/minirpc/parent/minirpc_client.cpp tests/minirpc_client_test.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add minirpc parent clients"
```

### Task 7: 为 `MiniRpc` 增加跨进程集成测试

**Files:**
- Create: `tests/minirpc_integration_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

覆盖：

- 父进程通过共享内存 RPC 调到子进程
- 同步和异步路径都能走通
- 子进程异常后下一次调用可恢复

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_integration_test && ctest --test-dir build --output-on-failure -R memrpc_minirpc_integration_test`
Expected:

- FAIL，集成接线尚未完成

**Step 3: Write minimal implementation**

补：

- 测试专用 bootstrap 接线
- 恢复路径验证

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_integration_test`
Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/minirpc_integration_test.cpp tests/CMakeLists.txt
git commit -m "test: add minirpc integration coverage"
```

### Task 8: 冻结 VPS 扩展并更新文档

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`
- Modify: `docs/plans/2026-03-09-framework-app-split-design.md`
- Modify: `docs/plans/2026-03-09-framework-app-split-implementation-plan.md`

**Step 1: Write the failing test**

这一步无自动化测试，使用文档审查作为护栏。

**Step 2: Run test to verify it fails**

不适用。

**Step 3: Write minimal implementation**

把文档同步到新的方向：

- 框架优先
- `MiniRpc` 先行
- VPS 暂停扩展
- 当前只维护 CMake
- 当前不引入代码生成
- 监听型能力后续走普通 `Poll...()` RPC

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure`
Expected:

- 全量 PASS

**Step 5: Commit**

```bash
git add docs/architecture.md docs/demo_guide.md docs/porting_guide.md docs/plans/2026-03-09-framework-app-split-design.md docs/plans/2026-03-09-framework-app-split-implementation-plan.md
git commit -m "docs: align plans with minirpc-first direction"
```

### Task 9: 最终验证

**Files:**
- Modify: 无

**Step 1: Run full verification**

Run: `cmake -S . -B build`
Expected:

- 配置成功

**Step 2: Build**

Run: `cmake --build build`
Expected:

- 构建成功

**Step 3: Test**

Run: `ctest --test-dir build --output-on-failure`
Expected:

- 全量 PASS

**Step 4: Inspect workspace**

Run: `git status --short`
Expected:

- 仅包含本次改动，无意外文件

**Step 5: Commit**

```bash
git add .
git commit -m "feat: add minirpc demo app and tighten framework boundaries"
```
