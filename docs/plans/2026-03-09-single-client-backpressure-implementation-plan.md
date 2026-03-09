# 单 Client/Server 背压与执行状态 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在单 client + 单 server 约束下，为 `memrpc` 增加 admission 背压、三段超时语义和 slot 级执行状态记录，并消除 server 无界 drain 共享队列的问题。

**Architecture:** 保留现有共享内存 session、request/response ring 和高优/普通 worker pool，总体上沿用当前框架。变更重点放在三处：一是在 client 侧增加 admission wait 和独立 timeout；二是在共享内存 slot 中补执行态元数据；三是把 server 的本地 worker queue 改成有界队列，使共享 request ring 重新成为系统真实背压点。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、pthread robust mutex

---

### Task 1: 补单 Client 显式约束测试

**Files:**
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `src/core/protocol.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 第一个 client attach 成功
- 第二个 client attach 同一 session 明确失败
- client shutdown 或 session reset 后，新的 attach 可以重新成功

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test'`

Expected:

- FAIL，因为当前实现默认允许重复 `Connect()` 并 attach 同一 session

**Step 3: Write minimal implementation**

最小实现：

- 在共享内存 header 增加单 client 占有标记
- `Session::Attach()` 在 client attach 路径上做显式占有校验
- `Session::Reset()` 或 client shutdown 时释放占有标记

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/session_test.cpp src/core/session.h src/core/session.cpp src/core/protocol.h src/bootstrap/posix_demo_bootstrap.cpp
git commit -m "feat: enforce single-client session ownership"
```

### Task 2: 补 admission timeout API 与基础测试

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/handler.h`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/types_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `RpcCall` 暴露 `admission_timeout_ms`
- 默认值符合设计
- `0` 被解释为无限等待语义

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_types_test'`

Expected:

- FAIL，因为当前没有 admission timeout 字段

**Step 3: Write minimal implementation**

最小实现：

- 在 `RpcCall` 中新增 `admission_timeout_ms`
- 只在 public API 和测试里先打通字段，不修改主路径逻辑

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_types_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h include/memrpc/server/handler.h tests/memrpc/rpc_client_api_test.cpp tests/memrpc/types_test.cpp
git commit -m "feat: add admission timeout api"
```

### Task 3: 补 slot 执行状态元数据与协议布局测试

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/core/shm_layout.h`
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- slot 中新增执行状态元数据后布局仍可计算
- 共享内存 header / slot 元数据字段初始值正确
- 状态枚举和字段大小保持固定

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test'`

Expected:

- FAIL，因为当前 slot 只包含 `RpcRequestHeader`

**Step 3: Write minimal implementation**

最小实现：

- 在 `SlotPayload` 前增加 `SlotRuntimeState`
- 字段至少包括 `request_id`、`state`、`worker_id`、`enqueue_mono_ms`、`start_exec_mono_ms`、`last_heartbeat_mono_ms`
- 更新 layout 相关计算和静态断言

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/core/shm_layout.h tests/memrpc/protocol_layout_test.cpp tests/memrpc/session_test.cpp
git commit -m "feat: add slot runtime state metadata"
```

### Task 4: 先补红灯测试，固定 server 不再无界 drain

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `src/server/rpc_server.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- worker 全忙且本地队列已满时，server 不再继续 drain 共享 request ring
- 共享 request ring 在过载时保持非空，client 会观察到 admission pressure

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_session_test'`

Expected:

- FAIL，因为当前 `DrainQueue()` 会持续搬空共享 request ring

**Step 3: Write minimal implementation**

最小实现：

- 为 `WorkerPool` 增加有界队列容量
- `Enqueue()` 在队列满时返回失败
- dispatcher 发现本地队列满时停止继续 drain 当前共享队列

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/session_test.cpp src/server/rpc_server.cpp
git commit -m "feat: bound server worker queue"
```

### Task 5: 实现 client admission wait

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 没有可用 slot 或 request ring 满时，client 在 `admission_timeout_ms` 内等待
- server 释放容量后，等待中的调用成功入队
- `admission_timeout_ms` 到期后返回预期错误

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- FAIL，因为当前 `Reserve()` 失败直接返回 `QueueFull`

**Step 3: Write minimal implementation**

最小实现：

- `InvokeAsync()` 对 slot 不足和 request ring 满改为 deadline 重试
- 引入轻量等待策略，例如 1 ms 到 2 ms sleep 重试
- `admission_timeout_ms == 0` 时持续重试直到成功或 session 死亡

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/core/slot_pool.h src/core/slot_pool.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: add client admission wait"
```

### Task 6: 接通 slot 状态机

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- client 入队前后 slot 状态从 `Admitted` 变为 `Queued`
- worker 开始执行时变为 `Executing`
- 回包写回时变为 `Responding`
- client 完成 request 后 slot 回到 `Free`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_session_test'`

Expected:

- FAIL，因为当前共享内存里没有完整状态机

**Step 3: Write minimal implementation**

最小实现：

- client 写 slot 时初始化 runtime state
- request push 成功后更新为 `Queued`
- server 开始执行前更新为 `Executing`
- 写回 reply 前更新为 `Responding`
- client 收到 reply 后释放 slot 并清理 runtime state

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.h src/core/session.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/session_test.cpp
git commit -m "feat: track slot execution state"
```

### Task 7: 补 queue timeout 与 exec timeout 分段测试

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `src/server/rpc_server.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 请求已进入共享 request ring，但因 server 繁忙超过 `queue_timeout_ms`，返回 `QueueTimeout`
- handler 实际执行超出 `exec_timeout_ms`，返回 `ExecTimeout`
- admission wait 成功后，`queue_timeout_ms` 和 `exec_timeout_ms` 仍按各自阶段独立生效

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到三段时间语义被正确切分

**Step 3: Write minimal implementation**

最小实现：

- 统一 `enqueue_mono_ms`、`start_exec_mono_ms` 的使用点
- 在 worker 真正开始执行前判定 `queue_timeout_ms`
- 在 handler 完成后判定 `exec_timeout_ms`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/rpc_server_api_test.cpp src/server/rpc_server.cpp
git commit -m "feat: split queue and exec timeout behavior"
```

### Task 8: 补 crash 现场状态测试

**Files:**
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- server crash 或 session broken 后，pending future 失败
- 共享内存 slot 元数据能保留最后已知状态
- 新 session 建立后不会误读旧 slot 元数据作为活动请求

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 crash 诊断状态与恢复语义兼容

**Step 3: Write minimal implementation**

最小实现：

- session broken 时避免过早抹掉 runtime state
- 新 session attach 后重新初始化自身可见状态
- 保持 pending future 的统一失败路径

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/session_test.cpp tests/memrpc/rpc_client_integration_test.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp
git commit -m "feat: preserve slot crash diagnostics"
```

### Task 9: 更新文档并全量验证

**Files:**
- Modify: `docs/architecture.md`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `include/memrpc/core/types.h`

**Step 1: Write the failing test**

确保文档与 API 注释同步：

- admission / queue / exec 三段 timeout 语义一致
- 单 client / 单 server 约束明确
- slot runtime state 的诊断用途明确

**Step 2: Run targeted verification**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- PASS，必要时在文档注释变更前出现失败

**Step 3: Write minimal implementation**

最小实现：

- 同步 API 注释与架构文档
- 避免文档仍描述旧的单段 timeout 或无界 server queue 行为

**Step 4: Run full verification**

Run: `cmake --build build`

Expected:

- BUILD PASS

Run: `ctest --test-dir build --output-on-failure`

Expected:

- 全部 PASS

Run: `./build/demo/memrpc_minirpc_demo`

Expected:

- demo 正常完成基础 request/reply 流程

**Step 5: Commit**

```bash
git add docs/architecture.md include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h include/memrpc/core/types.h
git commit -m "docs: describe single-client backpressure model"
```
