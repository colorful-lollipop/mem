# 单响应队列事件模型 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将额外通知收敛为“响应队列中的 `Event` 消息”，让 `MemRpc` 在一个响应队列和一个 `resp_eventfd` 上同时承载普通 RPC 回包和无头广播事件。

**Architecture:** 先用测试钉住“响应队列支持 `Reply/Event` 两种消息类型”的协议边界，再从框架中移除独立 notify 通道，改造 `RpcClient/RpcServer` 为统一的响应与事件分发模型。当前主构建继续只关注 `memrpc + minirpc`；`MiniRpc` 不使用事件，VPS 只在设计层预留基于框架事件的 listener 分发方式。

**Tech Stack:** C++17、shared memory、eventfd、GTest、CMake

---

### Task 1: 为单响应队列 `Reply/Event` 协议补失败测试

**Files:**
- Modify: `tests/protocol_layout_test.cpp`
- Modify: `tests/rpc_client_api_test.cpp`
- Modify: `tests/rpc_server_api_test.cpp`
- Modify: `tests/rpc_client_integration_test.cpp`
- Create: `tests/response_queue_event_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

补以下测试：

- 响应队列 entry 暴露 `message_kind`
- 客户端 API 暴露 `RpcEvent` 与 `SetEventCallback()`
- 服务端 API 暴露 `PublishEvent()`
- 单独的 `response_queue_event_test` 验证：
  - `Reply` 与 `Event` 能在同一响应队列中共存
  - `Event` 不需要命中 pending request

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，当前协议和 API 仍然是旧 notify 模型

**Step 3: Write minimal implementation**

只新增测试和最小编译占位，不先实现完整逻辑。

**Step 4: Run test to verify it still fails for the right reason**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，且失败集中在缺少 `Reply/Event` 协议和统一事件接口

**Step 5: Commit**

```bash
git add tests/protocol_layout_test.cpp tests/rpc_client_api_test.cpp tests/rpc_server_api_test.cpp tests/rpc_client_integration_test.cpp tests/response_queue_event_test.cpp tests/CMakeLists.txt
git commit -m "test: cover response queue event protocol"
```

### Task 2: 从框架中移除独立 notify 通道

**Files:**
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `include/memrpc/client/demo_bootstrap.h`
- Modify: `src/core/shm_layout.h`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `tests/session_test.cpp`
- Modify: `tests/sa_bootstrap_stub_test.cpp`
- Modify: `tests/bootstrap_callback_test.cpp`
- Delete: `tests/notify_channel_test.cpp`
- Delete: `tests/rpc_notify_integration_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

依赖以下断言：

- `BootstrapHandles` 不再包含 `notify_event_fd`
- 共享内存布局不再包含 notify ring
- session 不再暴露独立 notify 队列接口

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_sa_bootstrap_stub_test|memrpc_bootstrap_callback_test|memrpc_protocol_layout_test'`

Expected:

- FAIL，当前代码仍带独立 notify 通道

**Step 3: Write minimal implementation**

删除：

- `notify_event_fd`
- notify ring 布局
- `PushNotify/PopNotify`
- notify 专用测试与构造路径

保持：

- 高优请求队列
- 普通请求队列
- 单响应队列
- 单 `resp_eventfd`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_session_test|memrpc_sa_bootstrap_stub_test|memrpc_bootstrap_callback_test|memrpc_protocol_layout_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/bootstrap.h include/memrpc/client/demo_bootstrap.h src/core/shm_layout.h src/core/session.h src/core/session.cpp src/bootstrap/posix_demo_bootstrap.cpp tests/session_test.cpp tests/sa_bootstrap_stub_test.cpp tests/bootstrap_callback_test.cpp tests/CMakeLists.txt
git commit -m "refactor: remove standalone notify channel"
```

### Task 3: 将响应队列协议扩成 `Reply/Event`

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `src/core/shm_layout.h`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `tests/protocol_layout_test.cpp`
- Modify: `tests/response_queue_event_test.cpp`

**Step 1: Write the failing test**

扩展测试要求：

- `ResponseRingEntry` 具有 `message_kind`
- `Event` entry 具有 `event_domain/event_type/flags`
- `Reply` 和 `Event` 共用同一响应 ring

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，协议结构尚未变更

**Step 3: Write minimal implementation**

新增最小结构：

- `ResponseMessageKind`
- `ResponseRingEntry.message_kind`
- `event_domain`
- `event_type`
- `flags`

不新增新 ring，不新增新 slot 类型。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h src/core/shm_layout.h src/core/session.h src/core/session.cpp tests/protocol_layout_test.cpp tests/response_queue_event_test.cpp
git commit -m "feat: add reply and event kinds to response queue"
```

### Task 4: 提供通用框架事件 API

**Files:**
- Modify: `include/memrpc/core/types.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `tests/rpc_client_api_test.cpp`
- Modify: `tests/rpc_server_api_test.cpp`

**Step 1: Write the failing test**

新增断言：

- `RpcEvent` 可见
- `RpcClient::SetEventCallback()` 可调用
- `RpcServer::PublishEvent()` 可调用

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，API 仍是旧 notification 形状或尚不存在

**Step 3: Write minimal implementation**

新增：

- `struct RpcEvent`
- `using RpcEventCallback`
- `RpcClient::SetEventCallback()`
- `RpcServer::PublishEvent()`

删除或替换旧 `RpcNotification` 接口。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/core/types.h include/memrpc/client/rpc_client.h include/memrpc/server/rpc_server.h tests/rpc_client_api_test.cpp tests/rpc_server_api_test.cpp
git commit -m "feat: add generic response-queue event api"
```

### Task 5: 改造 `RpcClient` 为统一响应与事件分发

**Files:**
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/rpc_client_integration_test.cpp`
- Modify: `tests/response_queue_event_test.cpp`

**Step 1: Write the failing test**

覆盖：

- 普通回包仍能唤醒对应请求
- `Event` 不命中 pending map 也能触发客户端事件回调
- `Reply` 和 `Event` 混发时都正确

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，dispatcher 尚未理解 `message_kind`

**Step 3: Write minimal implementation**

在响应分发线程中：

- 先判断 `message_kind`
- `Reply` 走原路径
- `Event` 直接投递给事件回调

不改变普通同步等待模型。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_integration_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/rpc_client.cpp tests/rpc_client_integration_test.cpp tests/response_queue_event_test.cpp
git commit -m "feat: dispatch reply and event messages from one response queue"
```

### Task 6: 改造 `RpcServer` 通过响应队列发布事件

**Files:**
- Modify: `src/server/rpc_server.cpp`
- Modify: `tests/rpc_server_api_test.cpp`
- Modify: `tests/response_queue_event_test.cpp`

**Step 1: Write the failing test**

覆盖：

- `PublishEvent()` 能写入响应队列
- 事件写入后能通过 `resp_eventfd` 唤醒客户端

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_response_queue_event_test'`

Expected:

- FAIL，服务端仍未通过响应队列发布事件

**Step 3: Write minimal implementation**

实现：

- 服务端申请 slot
- 写入事件 payload
- 生成 `message_kind = Event` 的响应 entry
- `write(resp_eventfd, 1)`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_server_api_test|memrpc_response_queue_event_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/server/rpc_server.cpp tests/rpc_server_api_test.cpp tests/response_queue_event_test.cpp
git commit -m "feat: publish events through response queue"
```

### Task 7: 保持 `MiniRpc` 最小并更新文档

**Files:**
- Modify: `demo/minirpc_demo_main.cpp`
- Modify: `tests/minirpc_client_test.cpp`
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`

**Step 1: Write the failing test**

确认：

- `MiniRpc` 不依赖事件能力也能完整工作
- 文档不再描述独立 notify 通道主线
- 文档改为说明“响应队列支持 `Reply/Event`”

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_client_test|memrpc_api_headers_test|memrpc_framework_split_headers_test'`

Expected:

- FAIL，直到文档和演示都与新模型一致

**Step 3: Write minimal implementation**

更新：

- `MiniRpc` demo 保持只演示基础 RPC
- 文档描述统一为：
  - 单响应队列
  - 单 `resp_eventfd`
  - `Reply/Event` 双消息类型

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
git add demo/minirpc_demo_main.cpp tests/minirpc_client_test.cpp docs/architecture.md docs/demo_guide.md docs/porting_guide.md
git commit -m "docs: align response queue event model"
```
