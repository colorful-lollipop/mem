# Credit Eventfd Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 用 2 个新增 credit eventfd 去掉 request/response 侧的 `1ms` 轮询，同时把消息通知和 credit 通知都收敛成“状态跃迁才唤醒”，降低小 payload 高频 RPC 下的 eventfd syscall 和无效 wakeup 成本。

**Architecture:** 保留当前 `单 client + 单 server + 3 条 ring + request/response slot` 总体结构，不改共享内存布局。现有 3 个消息到达 fd 改成边沿通知，只在 `empty -> non-empty` 时写；新增 `reqCreditEventFd` 和 `respCreditEventFd` 两个 credit fd，并用 waiter-aware 的 armed 标志控制写入，只在对端确实因为资源不足而阻塞时唤醒。被唤醒的一侧继续批量 drain/flush，把一次唤醒摊到多条消息上。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、poll、condition_variable、slot pool、ring cursor

---

## 当前基线

- `highReqEventFd` / `normalReqEventFd` / `respEventFd` 目前基本按“每次成功 push 都 write”使用
- client submitter 在 request admission 失败时 `sleep_for(1ms)` 重试
- server response writer 在 response slot / response ring 资源不足时 `sleep_for(1ms)` 重试
- small-payload copy reduction 做完后，payload copy 固定成本下降，eventfd/poll 的固定 syscall 成本会更显眼
- 当前 ring 真相仍是共享内存 cursor，eventfd 只适合作为 wakeup hint

## 设计约束

- 不改变 request/response ring 和 slot 的共享内存 layout
- 不复用现有 3 个消息 fd 的语义
- 不把 eventfd 计数当作资源真相
- 不要求每次资源释放都精确对应一次 credit write
- 优先控制无效唤醒和 syscall 数量，再考虑更细粒度的水位优化

### Task 1: 给 5 个 fd 补齐句柄和测试护栏

**Files:**
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `tests/memrpc/session_test.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `BootstrapHandles` 新增 `reqCreditEventFd` 和 `respCreditEventFd`
- `PosixDemoBootstrapChannel::Connect()` / `server_handles()` 会复制这 2 个 fd
- `Session::Attach()` 后这 2 个 fd 对 client/server 可见且合法

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(session|rpc_client_api)_test'`

Expected:

- FAIL，因为当前句柄集合里还没有 credit fd

**Step 3: Write minimal implementation**

最小实现：

- 在 `BootstrapHandles` 增加 `reqCreditEventFd`、`respCreditEventFd`
- `PosixDemoBootstrapChannel` 创建、dup、关闭这 2 个 fd
- `Session` 只做透明 attach/reset，不引入行为改动

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(session|rpc_client_api)_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/bootstrap.h src/bootstrap/posix_demo_bootstrap.cpp src/core/session.h src/core/session.cpp tests/memrpc/session_test.cpp tests/memrpc/rpc_client_api_test.cpp
git commit -m "feat: add credit eventfds to bootstrap handles"
```

### Task 2: 把 3 个消息 fd 改成边沿通知

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/core/session.h`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- request ring 从空变非空时只写一次 `highReqEventFd` / `normalReqEventFd`
- response ring 从空变非空时只写一次 `respEventFd`
- 连续 push 多条消息时，不会因为每条消息都 write 而产生额外 eventfd 计数

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(rpc_client_integration|response_queue_event)_test'`

Expected:

- FAIL，因为当前成功 push 后仍是每次都 write

**Step 3: Write minimal implementation**

最小实现：

- 在 `Session` 或调用侧增加“push 前 ring 是否为空”的判断辅助
- `RpcClient::SubmitOne()` 只在目标 request ring `empty -> non-empty` 时写消息 fd
- `RpcServer::ResponseWriterLoop()` 只在 response ring `empty -> non-empty` 时写 `respEventFd`
- 仍然保持 consumer 被唤醒后 drain 到空

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(rpc_client_integration|response_queue_event)_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/server/rpc_server.cpp src/core/session.h tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "perf: edge-trigger shared ring eventfds"
```

### Task 3: 给 response credit 通道加 waiter-aware 唤醒

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- response writer 因 `response ring` 满或 `response slot` 不足阻塞后，client 消费 reply/event 会通过 `respCreditEventFd` 唤醒它
- client 连续消费多个 response 时，不会对每次 `PopResponse()` 和每次 `Release(response_slot)` 都盲写 credit fd
- 只有 response writer 真的 armed 等待时，client 才写 `respCreditEventFd`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(response_queue_event|rpc_client_integration)_test'`

Expected:

- FAIL，因为当前 response writer 仍依赖 `sleep_for(1ms)`，也没有 waiter-aware credit 语义

**Step 3: Write minimal implementation**

最小实现：

- 在 `RpcServer::Impl` 增加 `response_credit_waiting` 原子标志
- `ResponseWriterLoop()` 在资源不足前 arm 标志并阻塞 `poll(respCreditEventFd, ...)`
- client response loop 在：
  - `PopResponse()` 让 response ring 从满变非满
  - `response_slot_pool.Release()` 让 free count 从 `0 -> >0`
  时，只在看到 `response_credit_waiting` 为 true 时写 credit fd
- 被唤醒后 response writer 清 armed 标志并批量重试 flush

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(response_queue_event|rpc_client_integration)_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp src/client/rpc_client.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: replace response retry polling with credit eventfd"
```

### Task 4: 给 request credit 通道加 waiter-aware 唤醒

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- submitter 因 request ring 满或 request slot 不足阻塞后，server dispatcher `PopRequest()` / client 完成 reply 后释放 request slot 会通过 `reqCreditEventFd` 唤醒 submitter
- submitter 不再依赖 `sleep_for(1ms)` 重试 admission
- 只有 submitter 真的 armed 等待时，server/client 才写 `reqCreditEventFd`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- FAIL，因为当前 submitter 仍然轮询 admission

**Step 3: Write minimal implementation**

最小实现：

- 在 `RpcClient::Impl` 增加 `request_credit_waiting` 原子标志
- `SubmitOne()` 在 admission 失败时 arm 标志并阻塞等待 `reqCreditEventFd`
- `RpcServer::DrainQueue()` / dispatcher 成功 `PopRequest()` 后，如果看到 `request_credit_waiting` 为 true，则写 credit fd
- `RpcClient::CompleteRequest()` 在释放 request slot 且看到 armed 标志时写 credit fd

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: replace request admission polling with credit eventfd"
```

### Task 5: 把唤醒后的处理收敛成批量 drain/flush

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 一次消息 fd 唤醒后，consumer 会把当前 ring 尽量 drain 到空
- 一次 credit fd 唤醒后，submitter/response writer 会尽量批量处理多个请求/回包，而不是只推进 1 个
- 在 burst 场景下，eventfd 计数增长低于消息数，仍能正确完成全部请求

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(rpc_client_integration|response_queue_event)_test'`

Expected:

- FAIL，直到 loop 变成批量推进而非单步推进

**Step 3: Write minimal implementation**

最小实现：

- submitter 被 `reqCreditEventFd` 唤醒后，优先在本轮尽量推进后续 submission
- response writer 被 `respCreditEventFd` 唤醒后，优先在本轮尽量 flush completion backlog
- 保持现有公平性：高优 request 仍优先于普通 request

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(rpc_client_integration|response_queue_event)_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "perf: batch progress after eventfd wakeups"
```

### Task 6: 文档同步和全量验证

**Files:**
- Modify: `docs/plans/2026-03-09-credit-eventfd-design.md`
- Modify: `docs/architecture.md`
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`

**Step 1: Write the failing test**

这一步不新增失败测试，改做文档和接口对齐检查：

- credit fd 语义与实现一致
- message fd 的边沿通知语义写进文档
- runtime stats 或中文注释补齐新的等待/唤醒逻辑

**Step 2: Run test to verify current state**

Run: `cmake --build build`

Expected:

- PASS

**Step 3: Write minimal implementation**

最小实现：

- 更新设计文档和架构文档
- 如果实现里新增了统计或注释缺口，补到公开头和关键实现点

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_(session|rpc_client_api|rpc_server_api|rpc_client_integration|response_queue_event)_test|memrpc_minirpc_(headers|codec|service|client)_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add docs/plans/2026-03-09-credit-eventfd-design.md docs/architecture.md include/memrpc/core/bootstrap.h include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h src/client/rpc_client.cpp src/server/rpc_server.cpp tests/memrpc/session_test.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "docs: align credit eventfd wakeup behavior"
```
