# Eventfd Wakeup Reduction Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在不改变当前 `3 eventfd + ring + slot` 协议边界的前提下，减少请求和响应路径上的 `eventfd` 写次数与空唤醒，优先降低突发流量下的 syscall 放大。

**Architecture:** 保留现有单生产者/单消费者共享 ring 结构，继续让 client submitter、server dispatcher、server response writer、client response dispatcher 分别承担唯一写/读角色。本次优化不减少 fd 数量，而是在生产侧增加“队列从空变非空时才触发唤醒”的边沿策略，并补齐计数、测试和回退护栏。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、poll

---

## 当前基线

- 当前只有 3 个 `eventfd`：`high_req_eventfd`、`normal_req_eventfd`、`resp_eventfd`
- client 请求路径在每次 `PushRequest()` 成功后立即 `write(req_eventfd, 1)`
- server 响应路径在每次 `PushResponse()` 成功后立即 `write(resp_eventfd, 1)`
- 消费侧已经会批量 drain `eventfd` 计数和共享 ring，但生产侧没有做唤醒合并
- server dispatcher 已经支持在 worker 释放容量后主动回看 shared ring，不完全依赖新信号

## 约束

- 不引入新的共享 fd，不恢复独立 notify 通道
- 不破坏当前 `Reply/Event` 共用 `resp_eventfd` 的模型
- 不依赖“eventfd 计数必须等于 ring entry 数量”的语义
- 优先保证“不漏唤醒”，其次才是减少 syscall

### Task 1: 补事件唤醒观测点与红灯测试

**Files:**
- Modify: `src/core/session_test_hook.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 多个普通请求连续入队时，请求侧不要求“一次请求对应一次 eventfd write”
- 多个回包/事件连续写入时，响应侧不要求“一条消息对应一次 eventfd write”
- 即使减少 write 次数，server/client 仍能把共享 ring 中的残留项 drain 完

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，因为当前实现仍是每次成功入队都立刻写 `eventfd`

**Step 3: Write minimal implementation**

最小实现：

- 增加测试专用 `eventfd write` 计数 hook
- 先只做可观测性，不改生产逻辑

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- PASS，且测试能稳定观测请求侧/响应侧写次数

**Step 5: Commit**

```bash
git add src/core/session_test_hook.h src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "test: add eventfd wakeup observability"
```

### Task 2: 请求侧改为边沿唤醒

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `high request ring` 从空变非空时写一次 `high_req_eventfd`
- `normal request ring` 从空变非空时写一次 `normal_req_eventfd`
- ring 已非空且 submitter 持续推入时，不重复写 fd
- server dispatcher 被唤醒一次后，仍可继续 drain 同轮突发写入的请求

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- FAIL，因为当前代码每次 `PushRequest()` 成功都会 `write()`

**Step 3: Write minimal implementation**

最小实现：

- 在 submitter 线程里于 `PushRequest()` 之前读取目标 ring 是否为空
- 仅在“推入前为空且推入成功”时写对应 `req_eventfd`
- 保持 `QueueFull` / session death / pending 清理逻辑不变

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: coalesce request eventfd wakeups"
```

### Task 3: 响应侧改为边沿唤醒

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response ring 从空变非空时只写一次 `resp_eventfd`
- 连续回包和事件批量入队时不再逐条写 fd
- client response loop 被唤醒一次后仍能 drain reply/event 混合流量

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，因为当前 response writer 每次 `PushResponse()` 成功都会 `write()`

**Step 3: Write minimal implementation**

最小实现：

- 在 response writer 推入 response ring 前判断 ring 是否为空
- 仅在“推入前为空且推入成功”时写 `resp_eventfd`
- 保留 `MarkSessionBroken()` 的强制唤醒路径，不与正常回包合并

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: coalesce response eventfd wakeups"
```

### Task 4: 校验极端场景与回退边界

**Files:**
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- poll 超时空转后，下一次“空到非空”仍能重新触发唤醒
- response ring 已有数据但 client 尚未 drain 完时，后续追加消息不会丢
- `eventfd` 饱和或写失败时，现有 `PeerDisconnected` / broken session 语义不回退

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到边沿唤醒策略经过异常路径校验

**Step 3: Write minimal implementation**

最小实现：

- 修正边沿判断中的竞态或恢复缺口
- 只在必要时增加轻量状态读取辅助函数，不引入新线程或新协议字段

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_session_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/response_queue_event_test.cpp tests/memrpc/session_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "test: harden eventfd wakeup coalescing edge cases"
```

### Task 5: 补压测指标与文档

**Files:**
- Modify: `docs/architecture.md`
- Modify: `demo/`
- Modify: `tests/memrpc/`

**Step 1: Write the failing test**

补验证覆盖：

- 在固定批量请求下，记录优化前后 `eventfd write` 次数
- 输出每请求平均 write 次数、每回复平均 write 次数、总 syscall 下降比例

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，直到指标输出路径或文档更新完成

**Step 3: Write minimal implementation**

最小实现：

- 在文档中明确：`eventfd` 只承担“有新活可看”的边沿提示，不保证一条消息一个信号
- 为 demo 或测试增加简单计数输出，便于回归比较

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add docs/architecture.md demo tests/memrpc
git commit -m "docs: record coalesced eventfd wakeup semantics"
```
