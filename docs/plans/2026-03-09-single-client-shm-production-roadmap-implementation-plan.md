# 单 Client/Server 共享内存 RPC 生产化路线图 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在单 client + 单 server 约束下，分阶段收紧 `memrpc` 的背压、超时、在途状态和协议演进路径，使其更接近生产级实现。

**Architecture:** 第一阶段不大改共享内存协议主体，先把单 client ownership、client admission 等待、server 有界背压、slot 状态记录和同步等待机制收紧。第二阶段再推进响应路径从内嵌 ring payload 演进到 `response ring + response slot` 的对称模型。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、pthread robust mutex

---

### Task 1: 固化单 Client Ownership 语义

**Files:**
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `src/bootstrap/sa_bootstrap.cpp`
- Modify: `tests/memrpc/bootstrap_callback_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 第二个 client 尝试 attach 同一活跃 session 时被拒绝
- 原 client shutdown 或 session death 后，新 client 才能重新 attach

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_bootstrap_callback_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 bootstrap 层显式执行单 client ownership

**Step 3: Write minimal implementation**

在 bootstrap 层为单 session 增加一个明确的 active-client ownership 语义，不在 client 或 server 侧做隐式兜底。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_bootstrap_callback_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/bootstrap.h src/bootstrap/posix_demo_bootstrap.cpp src/bootstrap/sa_bootstrap.cpp tests/memrpc/bootstrap_callback_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: enforce single client session ownership"
```

### Task 2: 引入 Admission 等待与第三类超时

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/core/types.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 当 `slot` 不可用或 request ring 满时，client 会等待而不是立即失败
- `admission_timeout_ms` 到期后返回明确失败
- `admission_timeout_ms = 0` 表示可无限等待 admission

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 client admission 等待机制落地

**Step 3: Write minimal implementation**

新增 `admission_timeout_ms`，并在 client 的 slot reserve / request enqueue 路径中实现有界重试等待。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h include/memrpc/core/types.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: add client admission wait timeout"
```

### Task 3: 把 Server 背压改成有界

**Files:**
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 当 worker 全忙且本地等待容量耗尽时，server 不再继续 drain 共享请求队列
- 共享请求队列满后，client admission 等待路径被触发

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 server 从无界进程内排队改成有界背压

**Step 3: Write minimal implementation**

将 `WorkerPool` 前的 server 本地等待队列做成显式有界，或删除中间无界队列，确保共享请求队列成为真正背压点。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/server/rpc_server.h src/server/rpc_server.cpp tests/memrpc/rpc_server_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: make server backpressure bounded"
```

### Task 4: 在 Request Slot 中记录在途状态

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- request slot 能记录 `request_id` 和关键状态阶段
- 请求从 reserved 到 queued 到 executing 到 completed 的状态转换正确
- engine death 后，残留 slot 状态可用于诊断

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 slot 元数据和状态迁移完整

**Step 3: Write minimal implementation**

扩展 request slot 元数据，优先增加：

- `request_id`
- `state`
- `worker_id`
- `enqueue_mono_ms`
- `start_exec_mono_ms`
- `last_heartbeat_mono_ms`

只实现本轮需要的状态记录和迁移，不提前设计复杂诊断格式。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.h src/core/session.cpp tests/memrpc/protocol_layout_test.cpp tests/memrpc/session_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: record request slot in-flight state"
```

### Task 5: 去掉同步等待 Busy Wait

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `InvokeSync()` 在有限 deadline 下不依赖 1ms 轮询
- 超时时能按新的三段超时预算稳定返回

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到同步等待路径改成条件变量 deadline wait

**Step 3: Write minimal implementation**

为 `RpcFuture` 增加 deadline wait 能力，避免 `InvokeSync()` 自己 busy wait。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "fix: remove sync wait busy loop"
```

### Task 6: 文档化三段超时和单 Client 语义

**Files:**
- Modify: `docs/architecture.md`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `include/memrpc/core/bootstrap.h`

**Step 1: Write the failing test**

确保接口和注释能清楚表述：

- 单 client ownership
- `admission_timeout_ms`
- `queue_timeout_ms`
- `exec_timeout_ms`
- 当前 `exec_timeout_ms` 是软超时

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- 如接口与文档未对齐则 FAIL

**Step 3: Write minimal implementation**

同步更新对外头文件和架构文档，避免语义仍停留在旧模型。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add docs/architecture.md include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h include/memrpc/core/bootstrap.h
git commit -m "docs: describe single-client timeout model"
```

### Task 7: 第二阶段协议对称化预研

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Create: `docs/plans/2026-03-09-response-slot-design.md`

**Step 1: Write the failing test**

先补针对 response slot 的协议与行为测试：

- 响应体不再内嵌在 response ring
- response ring 只保存索引与元数据
- client 读完响应后释放 response slot

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到响应路径协议对称化完成

**Step 3: Write minimal implementation**

这一任务不要求与 P0 同轮执行。先单独出设计，再在独立分支推进。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.h src/core/session.cpp tests/memrpc/protocol_layout_test.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp docs/plans/2026-03-09-response-slot-design.md
git commit -m "feat: prototype response slot protocol"
```

### Task 8: 全量验证

**Files:**
- Modify: `docs/plans/2026-03-09-single-client-shm-production-roadmap-design.md`
- Modify: `docs/plans/2026-03-09-single-client-shm-production-roadmap-implementation-plan.md`

**Step 1: Run build**

Run: `cmake --build build`

Expected:

- BUILD PASS

**Step 2: Run focused framework tests**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(api_headers|framework_split_headers|protocol_layout|session|rpc_client_api|rpc_client_integration|rpc_server_api|response_queue_event)_test'`

Expected:

- PASS

**Step 3: Run full suite**

Run: `ctest --test-dir build --output-on-failure`

Expected:

- PASS

**Step 4: Run demo**

Run: `./build/demo/memrpc_minirpc_demo`

Expected:

- 基础跨进程 RPC 正常输出

**Step 5: Commit**

```bash
git add docs/plans/2026-03-09-single-client-shm-production-roadmap-design.md docs/plans/2026-03-09-single-client-shm-production-roadmap-implementation-plan.md
git commit -m "docs: add single-client shm production roadmap"
```
