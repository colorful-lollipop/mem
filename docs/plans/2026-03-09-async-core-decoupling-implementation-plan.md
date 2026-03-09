# 异步内核与应用层解耦 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将当前共享内存框架收敛成“异步一等公民、同步兼容包装、应用层示例化”的结构，避免应用 demo 反向污染框架公共接口。

**Architecture:** 在保留现有共享内存、slot、双优先级队列和恢复逻辑的基础上，新增公共 `RpcClient/RpcFuture/RpcServer` 抽象，把 `EngineClient/EngineServer` 退化为兼容层；应用层如 `VirusEngineManager` 仅作为示例建立在公共接口之上。

**Tech Stack:** C++17、shared memory、eventfd、GTest、CMake、GN

---

### Task 1: 为公共异步客户端接口增加失败测试

**Files:**
- Create: `tests/rpc_client_api_test.cpp`
- Create: `include/memrpc/rpc_client.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `BUILD.gn`

**Step 1: Write the failing test**

覆盖：

- `RpcCall`
- `RpcReply`
- `RpcFuture`
- `RpcClient`

至少验证头文件可组合和基础方法存在。

**Step 2: Run test to verify it fails**

Run: `cmake -S . -B build && cmake --build build --target memrpc_rpc_client_api_test`

Expected:

- FAIL，头文件不存在

**Step 3: Write minimal implementation**

新增公共头文件，先只放类型和最小类声明。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_api_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/rpc_client.h tests/rpc_client_api_test.cpp tests/CMakeLists.txt BUILD.gn
git commit -m "feat: add rpc client public api"
```

### Task 2: 为 `InvokeAsync/InvokeSync` 公共行为增加失败测试

**Files:**
- Create: `tests/rpc_client_integration_test.cpp`
- Modify: `src/client/engine_client.cpp`
- Create: `src/client/rpc_client.cpp`

**Step 1: Write the failing test**

覆盖：

- `InvokeAsync()` 能提交请求
- `Wait()` 能得到 reply
- `InvokeSync()` 与 `InvokeAsync()+Wait()` 行为一致

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_rpc_client_integration_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

将现有内部 `Invoke()` 抽到公共 `RpcClient`，并实现 `RpcFuture`。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_client_integration_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/client/engine_client.cpp tests/rpc_client_integration_test.cpp
git commit -m "refactor: promote async rpc client interface"
```

### Task 3: 为公共服务端抽象增加失败测试

**Files:**
- Create: `include/memrpc/rpc_server.h`
- Create: `tests/rpc_server_api_test.cpp`
- Modify: `src/server/engine_server.cpp`

**Step 1: Write the failing test**

覆盖：

- `RpcServer`
- `RegisterHandler()`
- `Start()/Stop()`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_rpc_server_api_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

把现有 `EngineServer` 的公共通用部分抽成 `RpcServer`。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_server_api_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/rpc_server.h src/server/engine_server.cpp tests/rpc_server_api_test.cpp
git commit -m "refactor: expose rpc server public api"
```

### Task 4: 为公共事件通道增加失败测试

**Files:**
- Create: `tests/rpc_event_integration_test.cpp`
- Create: `include/memrpc/rpc_event.h`
- Create: `src/core/rpc_event.cpp`
- Modify: `src/core/protocol.h`

**Step 1: Write the failing test**

覆盖：

- 服务端发布事件
- 客户端消费事件
- session 死亡后事件流停止

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_rpc_event_integration_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

增加公共事件 ring 和事件消费接口。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_rpc_event_integration_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/rpc_event.h src/core/rpc_event.cpp src/core/protocol.h tests/rpc_event_integration_test.cpp
git commit -m "feat: add rpc event channel"
```

### Task 5: 将 `EngineClient/EngineServer` 退化为兼容层

**Files:**
- Modify: `include/memrpc/client.h`
- Modify: `include/memrpc/server.h`
- Modify: `src/client/engine_client.cpp`
- Modify: `src/server/engine_server.cpp`

**Step 1: Write the failing test**

调整现有集成测试，要求：

- `EngineClient` 继续可用
- `EngineServer` 继续可用
- 其内部实际走新的公共 `RpcClient/RpcServer`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

让 `EngineClient/EngineServer` 只作为兼容包装层存在。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client.h include/memrpc/server.h src/client/engine_client.cpp src/server/engine_server.cpp
git commit -m "refactor: make engine api a compatibility layer"
```

### Task 6: 让 VPS demo 仅依赖公共 RPC 接口

**Files:**
- Modify: `include/vps_demo/virus_engine_manager.h`
- Modify: `src/vps_demo/virus_engine_manager.cpp`
- Modify: `src/vps_demo/virus_engine_service.cpp`

**Step 1: Write the failing test**

覆盖：

- `VirusEngineManager` 不直接依赖框架私有实现细节
- 只通过公共 `RpcClient/RpcServer` 与框架交互

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R \"memrpc_vps_manager_integration_test|memrpc_vps_listener_integration_test\"`

Expected:

- FAIL

**Step 3: Write minimal implementation**

替换应用层对框架内部细节的直接依赖。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R \"memrpc_vps_manager_integration_test|memrpc_vps_listener_integration_test\"`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/vps_demo/virus_engine_manager.h src/vps_demo/virus_engine_manager.cpp src/vps_demo/virus_engine_service.cpp
git commit -m "refactor: decouple vps demo from framework internals"
```

### Task 7: 文档与最终验证

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/porting_guide.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/plans/2026-03-09-vps-manager-compat-design.md`

**Step 1: Re-read design alignment**

确认：

- 框架层不再被应用层绑死
- 异步接口是一等能力
- 同步接口是兼容包装

**Step 2: Run full verification**

Run:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/demo/memrpc_demo_dual_process`
- `./build/demo/vps_manager_demo_main`

**Step 3: Final commit**

```bash
git add docs/architecture.md docs/porting_guide.md docs/demo_guide.md docs/plans/2026-03-09-vps-manager-compat-design.md
git commit -m "docs: describe async rpc core layering"
```
