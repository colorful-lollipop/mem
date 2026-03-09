# 双 SPSC + 统一 Slot 共享内存 RPC 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `memrpc` 演进为 3 条 lock-free `SPSC` 共享 ring、请求/响应统一使用 slot 的共享内存 RPC 架构，并在同一过程中收紧背压、超时与在途状态语义。

**Architecture:** 第一阶段先把跨进程边界改造成 3 条 lock-free `SPSC` 共享 ring，把生产者/消费者线程角色收敛；第二阶段引入 `response slot pool`，让请求和响应统一成 `ring + slot`；第三阶段补齐 admission timeout、高优 slot 保留、slot 在途状态和过载行为验证。共享 ring 做 lock-free，slot pool 和本地队列保留细粒度锁；进程 death/session 失效感知依赖外部鸿蒙框架。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、pthread robust mutex、线程池

---

## 当前基线

当前分支已经落了部分配套语义，但还没有完成“双 SPSC + 统一 response slot”目标：

- 已有：请求方向 `request slot`、`admission_timeout_ms`、`queue_timeout_ms` / `exec_timeout_ms`、reply/event 共用 `response ring`、server 有界本地 worker 队列、request slot 运行时状态字段
- 未有：client 专用发送线程、server 专用 response writer 线程、3 条共享 ring 的 lock-free `SPSC` cursor、`response slot pool`、高优 request slot 保留额度、去掉同步等待里的轮询

执行时按本计划顺序推进，不要假设当前分支里的“部分实现”已经满足最终语义；尤其是 Task 6 和 Task 7 需要在 Task 2 到 Task 5 完成后重新校准并补测。

## 执行约束

- 不回退当前已通过的 reply/event 兼容语义；`response ring` 在切到 `response slot` 前仍需同时承载 `Reply` 与 `Event`
- `slot_pool` 和 `session` 改动必须一起看，因为 slot 生命周期、共享内存布局和 ring entry 格式会联动变化
- `docs/architecture.md` 只在 Task 9 同步，前序任务以测试和实现收敛为主
- 每个任务完成后都先跑该任务列出的聚焦测试，再考虑进入下一任务

---

### Task 1: 补 3 条 Lock-Free SPSC Ring 目标结构的设计护栏测试

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- client 侧请求最终都经过统一发送路径
- server 侧响应最终都经过统一回包路径
- worker 不直接写共享响应队列
- 3 条共享 ring 的生产者/消费者角色固定为单写单读

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到请求/响应边界被统一收敛并为 lock-free SPSC 改造建立护栏

**Step 3: Write minimal implementation**

先加最小可验证护栏，确保后续结构重构不会回到“多线程直接写共享队列”的形态。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_server_api_test.cpp
git commit -m "test: add double spsc architecture guards"
```

### Task 2: 引入 Client 发送线程并收敛请求方向为 SPSC

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 多个业务线程并发调用 `InvokeAsync()` 时，请求仍按统一发送线程写共享请求队列
- 高优与普通请求都能经由该统一路径入队

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 client 发送线程与本地提交队列落地

**Step 3: Write minimal implementation**

增加 client 本地提交队列与发送线程，使共享请求 ring 的写入只发生在一个线程中。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: serialize request ring writes through tx thread"
```

### Task 3: 引入 Server 回包线程并收敛响应方向为 SPSC

**Files:**
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- worker 与 `PublishEvent()` 不再直接写共享响应队列
- 统一由 response writer 线程写 `response ring`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 server response writer 落地

**Step 3: Write minimal implementation**

增加 server 本地 completion queue 与 response writer 线程，使共享响应 ring 的写入只发生在一个线程中。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/server/rpc_server.h src/server/rpc_server.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: serialize response ring writes through writer thread"
```

### Task 4: 把 3 条共享 Ring 从 Mutex 版替换为 Lock-Free SPSC

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `src/core/shm_layout.h`
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/protocol_layout_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `high request ring` 只允许单生产者、单消费者访问语义
- `normal request ring` 只允许单生产者、单消费者访问语义
- `response ring` 只允许单生产者、单消费者访问语义
- ring 在空、满、回绕情况下行为正确

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_protocol_layout_test'`

Expected:

- FAIL，直到共享 ring 完成 lock-free SPSC 替换

**Step 3: Write minimal implementation**

移除 ring 对 mutex push/pop 的依赖，改成基于原子 `head/tail` 的 lock-free `SPSC` 实现。slot pool、本地队列和线程池不在本任务中改为 lock-free。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_protocol_layout_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/core/session.h src/core/session.cpp tests/memrpc/session_test.cpp tests/memrpc/protocol_layout_test.cpp
git commit -m "feat: replace shared rings with lock-free spsc"
```

### Task 5: 引入 Response Slot Pool，实现协议对称化

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `src/core/shm_layout.h`
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/slot_pool_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `response ring` 不再内嵌 payload
- `response slot` 承载回包正文
- client 读完回包后释放 `response slot`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_slot_pool_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到响应路径切换为 `ring + slot`

**Step 3: Write minimal implementation**

新增 `response slot pool` 和配套索引流程，把回包正文从 response ring 内联 payload 搬到 response slot 中。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_slot_pool_test|memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/core/session.h src/core/session.cpp src/core/shm_layout.h src/core/slot_pool.h src/core/slot_pool.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/protocol_layout_test.cpp tests/memrpc/slot_pool_test.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: move response payloads into response slots"
```

### Task 6: 引入 Admission Timeout 与真实背压

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/core/types.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- request slot 不可用或 request ring 满时，client 进入 admission 等待
- 当 worker 全忙且 server 本地有界队列满时，server 停止继续 drain 共享请求队列
- 超过 `admission_timeout_ms` 后明确失败

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到共享请求队列重新成为真实背压点

**Step 3: Write minimal implementation**

引入 `admission_timeout_ms`，并将 server 本地等待队列改为显式有界。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h include/memrpc/core/types.h src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/rpc_server_api_test.cpp
git commit -m "feat: add admission timeout and bounded backpressure"
```

### Task 7: 增加 Request/Response Slot 状态字段

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/slot_pool_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- request slot 状态从 reserve 到 execute 到 complete 的迁移正确
- response slot 状态从 reserve 到 ready 到 consumed 的迁移正确
- 调试卡顿或异常行为时，共享内存中残留状态可用于诊断

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_slot_pool_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到双 slot 状态元数据完整

**Step 3: Write minimal implementation**

为 request/response slot 增加状态、时间戳和必要的调试字段，不额外引入复杂调试协议。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_slot_pool_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/core/slot_pool.h src/core/slot_pool.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/protocol_layout_test.cpp tests/memrpc/session_test.cpp tests/memrpc/slot_pool_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: add unified slot state metadata"
```

### Task 8: 实现高优 Request Slot 保留额度

**Files:**
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/slot_pool_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 普通请求不能吃掉高优保留 request slot
- 高优请求在普通压力下仍能获得 request slot 并进入系统

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_slot_pool_test|memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到 request slot 保留额度生效

**Step 3: Write minimal implementation**

为 `request slot pool` 增加高优保留策略，不拆成高优/普通两套 request slot pool。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_slot_pool_test|memrpc_rpc_client_integration_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/server/rpc_server.h include/memrpc/client/rpc_client.h src/core/slot_pool.h src/core/slot_pool.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/slot_pool_test.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/rpc_server_api_test.cpp
git commit -m "feat: reserve request slots for high priority"
```

### Task 9: 去掉同步等待 Busy Wait 并同步文档

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `docs/architecture.md`

**Step 1: Write the failing test**

补测试覆盖：

- `InvokeSync()` 不再使用 1ms 轮询
- 接口注释明确三段超时和软 `exec_timeout_ms`
- 文档明确 crash / death 感知依赖外部鸿蒙框架

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- FAIL，直到同步等待和文档语义更新完成

**Step 3: Write minimal implementation**

为 future 增加 deadline wait，并同步公开头文件与架构文档。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h include/memrpc/core/bootstrap.h src/client/rpc_client.cpp docs/architecture.md
git commit -m "docs: describe double spsc unified slot model"
```

### Task 10: 全量验证

**Files:**
- Modify: `docs/plans/2026-03-09-double-spsc-unified-slot-design.md`
- Modify: `docs/plans/2026-03-09-double-spsc-unified-slot-implementation-plan.md`

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
git add docs/plans/2026-03-09-double-spsc-unified-slot-design.md docs/plans/2026-03-09-double-spsc-unified-slot-implementation-plan.md
git commit -m "docs: add double spsc unified slot plan"
```
