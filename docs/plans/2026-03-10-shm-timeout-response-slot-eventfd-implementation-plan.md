# 共享内存超时语义 + Response Slot 生命周期 + 去轮询 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修正三段超时语义、固定 4K payload 上限、强化 response slot 生命周期并去掉 response writer 轮询。

**Architecture:** 保持单 client / 单 server / 单 session 模型与 3 条 ring 不变；调整超时计算逻辑、response slot 生命周期规则，并用 credit eventfd 做资源等待；payload 上限固定为 4K 并更新协议版本。

**Tech Stack:** C++17, CMake, GTest, shared memory, eventfd, pthread mutex.

---

### Task 1: 固定 4K payload 上限与协议版本

**Files:**
- Modify: `include/memrpc/core/protocol.h`
- Modify: `src/core/session.cpp`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Test: `tests/memrpc/session_test.cpp`

**Step 1: Write the failing test**

在 `session_test` 中新增校验：
- `header->max_request_bytes == 4096`
- `header->max_response_bytes == 4096`
- `protocol_version` 自增后旧版本会被拒绝（模拟不同版本）。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_session_test`
Expected: FAIL with mismatch on max bytes or protocol version.

**Step 3: Write minimal implementation**

```cpp
inline constexpr uint32_t kProtocolVersion = 2u;
inline constexpr uint32_t kDefaultMaxRequestBytes = 4u * 1024u;
inline constexpr uint32_t kDefaultMaxResponseBytes = 4u * 1024u;
```

确保 `posix_demo_bootstrap` 的校验允许 4K 上限并写入 header。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_session_test`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/protocol.h src/core/session.cpp src/bootstrap/posix_demo_bootstrap.cpp tests/memrpc/session_test.cpp
git commit -m "feat: bump protocol version and fix payload limit to 4k"
```

### Task 2: 修正同步调用三段超时语义

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

新增测试覆盖：
- admission 超时仅影响 client 本地等待，返回 `QueueTimeout`；
- `InvokeSync` 预算 = admission + queue + exec，不额外 +1000ms；
- 三段均为 0 时为无限等待。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`
Expected: FAIL on timeout expectations.

**Step 3: Write minimal implementation**

```cpp
StatusCode RpcClient::InvokeSync(const RpcCall& call, RpcReply* reply) {
  if (reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  RpcFuture future = InvokeAsync(call);
  if (call.admission_timeout_ms == 0 && call.queue_timeout_ms == 0 && call.exec_timeout_ms == 0) {
    return future.Wait(reply);
  }
  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(call.admission_timeout_ms) +
      static_cast<int64_t>(call.queue_timeout_ms) +
      static_cast<int64_t>(call.exec_timeout_ms));
  return future.WaitFor(reply, wait_budget);
}
```

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`
Expected: PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "fix: align sync timeout budget with admission queue and exec phases"
```

### Task 3: 强化 response slot 生命周期与一致性校验

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

新增测试覆盖：
- ring 发布后，server 不会释放 response slot，即使 `respEventFd` 写失败；
- client 校验 `response_slot.runtime.request_id` 与 `entry.request_id`，不一致时返回 `ProtocolMismatch`；
- client 回收 response slot 后写 credit。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`
Expected: FAIL for slot lifetime or request_id mismatch.

**Step 3: Write minimal implementation**

关键改动：
- 在 `ResponseWriterLoop` 中，`PushResponse` 成功后不释放 slot，即使 `respEventFd` 写失败。
- `CompleteRequest` / `DeliverEvent` 增加 request_id 校验，失败则 `ProtocolMismatch` 并走断连路径。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_response_queue_event_test|memrpc_rpc_client_integration_test'`
Expected: PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp src/client/rpc_client.cpp tests/memrpc/response_queue_event_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "fix: enforce response slot lifetime and request id validation"
```

### Task 4: 去掉 response writer 固定轮询

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

新增测试覆盖：
- response slot/ring 满载时 writer 阻塞等待 `respCreditEventFd`；
- client 释放资源后 writer 立即继续（无 1ms 轮询）。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_response_queue_event_test`
Expected: FAIL due to polling behavior.

**Step 3: Write minimal implementation**

改造：
- `WaitForResponseCredit` 使用一次性 `poll(deadline)`，不再 `min(remaining, 100ms)` 循环。
- `ReserveResponseSlotWithRetry` / `PushResponseWithRetry` 只在资源不足时阻塞等待 credit，无 fixed sleep。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_response_queue_event_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp tests/memrpc/response_queue_event_test.cpp
git commit -m "perf: make response writer credit wait event-driven"
```

### Task 5: 文档同步

**Files:**
- Modify: `docs/architecture.md`

**Step 1: Write the failing test**

无测试，按文档约束更新。

**Step 2: Write minimal implementation**

补充：
- 4K payload 上限
- response slot 生命周期
- response writer 无轮询

**Step 3: Commit**

```bash
git add docs/architecture.md
git commit -m "docs: update timeout and response slot semantics"
```

---

Plan complete and saved to `docs/plans/2026-03-10-shm-timeout-response-slot-eventfd-implementation-plan.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
