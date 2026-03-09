# 框架核心稳定性测试补强 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 `memrpc` 主线补强恢复语义、队列边界和并发正确性的核心测试护栏。

**Architecture:** 先从恢复语义开始，用现有 `bootstrap/session/rpc_client` 集成链路验证 session replacement 的正确性；再补请求队列和 payload 上限边界；最后补多线程并发与 `Reply/Event` 混发正确性。测试尽量复用现有 `tests/memrpc/*` 文件，不扩成慢速大 E2E。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd

---

### Task 1: 补恢复语义失败测试

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/bootstrap_callback_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `pending request` 在死亡回调后立刻返回 `PeerDisconnected`
- 下一次 `InvokeSync()` 能在 bootstrap 重建后成功
- 新旧 `session_id` 不同

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_bootstrap_callback_test|memrpc_session_test'`

Expected:

- FAIL，直到恢复语义完整覆盖

**Step 3: Write minimal implementation**

仅修复被新测试暴露出的恢复链路缺口，不改无关逻辑。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_bootstrap_callback_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/bootstrap_callback_test.cpp tests/memrpc/session_test.cpp src/client/rpc_client.cpp src/bootstrap/posix_demo_bootstrap.cpp src/bootstrap/sa_bootstrap.cpp src/core/session.cpp src/core/session.h
git commit -m "test: cover session recovery behavior"
```

### Task 2: 补队列与上限边界测试

**Files:**
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 请求队列满时返回 `QueueFull`
- 请求 payload 超 `max_request_bytes` 时快速失败
- 事件 / 响应 payload 超配置上限时被拒绝

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到边界处理完整

**Step 3: Write minimal implementation**

只修复边界缺口，不改变主协议模型。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/session_test.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.cpp src/bootstrap/posix_demo_bootstrap.cpp
git commit -m "test: cover queue and payload limits"
```

### Task 3: 补高并发正确性测试

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 多线程并发 `InvokeAsync()` 不丢结果
- 高优短任务在普通积压下优先完成
- `Reply/Event` 混发不串包

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，直到并发路径稳定

**Step 3: Write minimal implementation**

只修复测试暴露出的并发正确性缺口。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.cpp
git commit -m "test: cover concurrent rpc correctness"
```

### Task 4: 更新文档并全量回归

**Files:**
- Modify: `docs/architecture.md`
- Modify: `AGENTS.md`

**Step 1: Write the failing test**

确保文档与当前主线测试边界一致：

- 测试优先覆盖恢复、边界、并发

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test|memrpc_framework_split_headers_test'`

Expected:

- 如文档未同步则 FAIL

**Step 3: Write minimal implementation**

同步文档中的测试重点和主线说明。

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
git add docs/architecture.md AGENTS.md
git commit -m "docs: describe framework stability test focus"
```
