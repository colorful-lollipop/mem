# 工业级可用性差距 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 收敛当前 `memrpc` 到可进入生产试运行的工程状态，优先修复验收中发现的 `P0` 语义漏洞，并补齐失败路径测试。

**Architecture:** 保持当前“单 client + 单 server + 单 session、3 条共享 ring、request/response slot”总体结构不变，不做新的大重构。本计划压缩为 2 个阶段：第一阶段一次性收口所有上线前必须修复的 `P0` 语义问题；第二阶段再做低拷贝、去轮询和可观测性增强。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、pthread mutex、线程池

---

## 范围说明

本计划按验收结论压成 2 个阶段：

- 阶段 1：上线前必须完成的 `P0`
- 阶段 2：进入长期生产化前建议完成的 `P1`

这份计划默认在当前分支继续推进，不新开架构分叉。

---

## 阶段 1：上线前必须完成

阶段 1 对应全部 `P0`，目标是把当前框架收敛到“可以进入生产试运行”的工程状态。只有这 5 个任务全部完成，才建议进入真实业务流量验证。

### Task 1: 修正同步调用的三段超时语义

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `InvokeSync()` 会把 `admission_timeout_ms` 纳入整体等待预算
- 本地 `WaitFor()` 超时不会错误折叠成 `PeerDisconnected`
- 同步路径在 admission 超时、queue 超时、exec 超时时和异步路径返回同类状态

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到同步等待预算和状态折叠逻辑被修正

**Step 3: Write minimal implementation**

- 明确同步等待预算包含 `admission_timeout_ms`
- 区分“本地等待超时”和“peer 断开”两类结果
- 保持 `InvokeAsync()` 为唯一真正提交入口，`InvokeSync()` 只做语义正确的包装

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "fix: align sync rpc timeouts with admission queue and exec phases"
```

### Task 2: 修正 response ring 发布后的 response slot 回收时序

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response ring entry 已发布后，即使 `eventfd write` 失败，也不会立刻释放仍可能被客户端读取的 `response slot`
- 客户端读 response ring 时不会拿到悬空 `slot_index`
- reply 和 event 两条路径都覆盖

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到 response slot 回收时序被收紧

**Step 3: Write minimal implementation**

- 重新定义“发布成功但通知失败”的 slot 生命周期
- 确保 response slot 只在客户端确认消费，或在明确不会再被消费时才释放
- 保持 `response ring` 和 `response slot` 的索引一致性

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp src/client/rpc_client.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "fix: harden response slot lifetime after ring publication"
```

### Task 3: 给 server completion queue 加有界容量和明确背压

**Files:**
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response writer 前的本地 completion queue 有明确上限
- 当 completion queue 满时，worker/event 发布方会得到可预期结果，而不是无限堆积
- 共享内存仍然是响应方向的主背压点，而不是 server 进程内存

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，直到 completion backlog 有界化

**Step 3: Write minimal implementation**

- 为 `completion_queue` 增加容量控制
- 明确满载时 reply/event 的处理策略
- 保证停机与失败路径下不会把 backlog 永久挂住

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/server/rpc_server.h src/server/rpc_server.cpp tests/memrpc/rpc_server_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "fix: bound server response completion backlog"
```

### Task 4: 加强 shared response slot pool 一致性保护

**Files:**
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `src/core/protocol.h`
- Modify: `tests/memrpc/slot_pool_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 重复释放 slot 会被拒绝
- 非法索引和陈旧释放不会污染 free list
- 压测下 `available_count` 与真实空闲状态一致

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_slot_pool_test|memrpc_session_test'`

Expected:

- FAIL，直到 shared slot pool 具有一致性校验

**Step 3: Write minimal implementation**

- 为 shared slot pool 增加 in-use bitmap、slot state 标记或 generation 防护中的一种最小实现
- 保持接口简单，不引入新的共享内存分配器

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_slot_pool_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/slot_pool.h src/core/slot_pool.cpp src/core/protocol.h tests/memrpc/slot_pool_test.cpp tests/memrpc/session_test.cpp
git commit -m "fix: add consistency guards to shared slot pool"
```

### Task 5: 补齐高风险失败路径测试

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response ring 已写入后通知失败
- sync admission timeout
- response slot double release / stale release
- completion backlog 满载时 reply/event 行为

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test|memrpc_rpc_server_api_test|memrpc_session_test'`

Expected:

- FAIL，直到所有关键失败路径都有稳定保护

**Step 3: Write minimal implementation**

- 只补让测试通过所需的最小修复
- 不在本任务顺手改动低拷贝或监控能力

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test|memrpc_rpc_server_api_test|memrpc_session_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_server_api_test.cpp tests/memrpc/session_test.cpp
git commit -m "test: cover critical failure paths for shared memory rpc"
```

---

## 阶段 2：工业化增强

阶段 2 对应 `P1`，这些任务不应该阻塞阶段 1 的试运行准入，但会直接影响后续的性能、维护性和运维排障效率。

### Task 6: 降低请求处理路径的整块拷贝

**Files:**
- Modify: `include/memrpc/server/handler.h`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `tests/apps/minirpc/minirpc_service_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试或基准护栏覆盖：

- server handler 可以直接读取 request slot view，而不是必须拷入新的 `std::vector<uint8_t>`
- 现有 handler 语义保持不变

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_minirpc_service_test'`

Expected:

- FAIL，直到 handler 输入模型支持低拷贝读取

**Step 3: Write minimal implementation**

- 给 `RpcServerCall` 增加只读 view 能力，或引入不破坏兼容性的轻量访问接口
- 避免一次额外整块 `assign`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_minirpc_service_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/server/handler.h src/server/rpc_server.cpp src/apps/minirpc/child/minirpc_service.cpp tests/apps/minirpc/minirpc_service_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: reduce request payload copies in server dispatch"
```

### Task 7: 去掉 response writer 的 1ms 轮询重试

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response slot 和 response ring 资源恢复时，response writer 能被条件唤醒
- 不依赖固定 1ms sleep 重试也能正确出队

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到等待方式更事件化

**Step 3: Write minimal implementation**

- 用条件变量或明确通知替代 `ReserveResponseSlotWithRetry()` / `PushResponseWithRetry()` 里的固定 sleep
- 保持错误处理和停止语义不变

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_server_api_test.cpp
git commit -m "perf: remove polling retries from response writer"
```

### Task 8: 补运行期观测与架构文档

**Files:**
- Modify: `docs/architecture.md`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`

**Step 1: Write the failing test**

补测试或接口护栏覆盖：

- 至少可查询或稳定记录 request/response ring 深度、slot 占用、completion backlog、三类 timeout 次数中的核心项
- 文档同步反映单 client 约束、Harmony 外部故障感知边界和 slot 生命周期

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，直到监控/接口与文档一致

**Step 3: Write minimal implementation**

- 暴露最小必要观测接口或稳定日志指标
- 同步更新 `docs/architecture.md`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add docs/architecture.md include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_server_api_test.cpp
git commit -m "docs: document and expose core memrpc runtime metrics"
```

### Task 9: 全量验证并收口生产试运行基线

**Files:**
- Modify: `docs/plans/2026-03-09-industrial-readiness-gap-implementation-plan.md`

**Step 1: Run focused test suites**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(session|protocol_layout|slot_pool|rpc_client_api|rpc_client_integration|response_queue_event|rpc_server_api)_test|memrpc_minirpc_(headers|codec|service|client)_test'`

Expected:

- PASS

**Step 2: Run full build**

Run: `cmake --build build`

Expected:

- PASS

**Step 3: Update plan status**

- 在本计划末尾补执行结果和未完成项
- 明确哪些 `P1` 仍保留给后续生产化增强

**Step 4: Commit**

```bash
git add docs/plans/2026-03-09-industrial-readiness-gap-implementation-plan.md
git commit -m "docs: record industrial readiness closure status"
```
