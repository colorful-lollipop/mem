# ScanBehavior 轮询通知 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `ScanBehavior` 的额外通知从独立 notify 通道收敛为普通 `PollBehaviorReports()` RPC，同时保持 `ScanBehavior()` 自身仍是普通 request/response 调用。

**Architecture:** 先用测试钉住“`ScanBehavior` 是普通 RPC、通知通过 `PollBehaviorReports()` 拉取”的新语义，再删除框架级 notify 通道，收缩 `MiniRpc`，最后在 VPS 应用层补齐轮询线程和 listener 广播。框架保持纯 request/response 模型，应用层自己定义通知队列和 `Poll...()` RPC。

**Tech Stack:** C++17、shared memory、eventfd、GTest、CMake

---

### Task 1: 为 `ScanBehavior + PollBehaviorReports()` 新语义补失败测试

**Files:**
- Modify: `tests/vps_service_test.cpp`
- Modify: `tests/vps_manager_integration_test.cpp`
- Create: `tests/vps_listener_polling_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

补三类测试：

- `ScanBehavior` 调用仍然立即返回 `SUCCESS/FAILED`
- 子进程在 `ScanBehavior` 后把 `BehaviorScanResult` 放入待拉取队列
- 主进程通过轮询 `PollBehaviorReports()` 获得结果并广播给 listener

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_service_test|memrpc_vps_manager_integration_test|memrpc_vps_listener_polling_test'`

Expected:

- FAIL，当前实现仍混有 notify 通道或缺少轮询线程路径

**Step 3: Write minimal implementation**

只补最小测试夹具和断言，不先改生产代码。

**Step 4: Run test to verify it still fails for the right reason**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_service_test|memrpc_vps_manager_integration_test|memrpc_vps_listener_polling_test'`

Expected:

- FAIL，且失败原因集中在 notify 逻辑尚未删除或轮询路径尚未实现

**Step 5: Commit**

```bash
git add tests/vps_service_test.cpp tests/vps_manager_integration_test.cpp tests/vps_listener_polling_test.cpp tests/CMakeLists.txt
git commit -m "test: cover polling-based behavior reports"
```

### Task 2: 删除框架级 notify 通道

**Files:**
- Modify: `include/memrpc/core/bootstrap.h`
- Modify: `include/memrpc/client/demo_bootstrap.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `src/core/protocol.h`
- Modify: `src/core/shm_layout.h`
- Modify: `src/core/session.h`
- Modify: `src/core/session.cpp`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`
- Delete: `tests/notify_channel_test.cpp`
- Delete: `tests/rpc_notify_integration_test.cpp`
- Modify: `tests/protocol_layout_test.cpp`
- Modify: `tests/session_test.cpp`
- Modify: `tests/sa_bootstrap_stub_test.cpp`
- Modify: `tests/bootstrap_callback_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

依赖以下现有测试和新断言：

- layout 不再包含 notify ring
- `BootstrapHandles` 不再暴露 `notify_event_fd`
- `RpcClient/RpcServer` 不再暴露 notification API

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_sa_bootstrap_stub_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- FAIL，当前代码仍包含 notify 结构和接口

**Step 3: Write minimal implementation**

删除：

- `notify_event_fd`
- notify ring 布局
- `RpcNotification`
- `RpcClient::SetNotificationCallback()`
- `RpcServer::PublishNotification()`
- 对应 dispatcher / publish 逻辑

保持核心 RPC 不变：

- 高优请求队列
- 普通请求队列
- 响应队列
- 响应 `eventfd`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_protocol_layout_test|memrpc_session_test|memrpc_sa_bootstrap_stub_test|memrpc_rpc_client_api_test|memrpc_rpc_server_api_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc src/core src/bootstrap/posix_demo_bootstrap.cpp src/client/rpc_client.cpp src/server/rpc_server.cpp tests/protocol_layout_test.cpp tests/session_test.cpp tests/sa_bootstrap_stub_test.cpp tests/bootstrap_callback_test.cpp tests/CMakeLists.txt
git commit -m "refactor: remove notify channel from memrpc core"
```

### Task 3: 收缩 `MiniRpc` 到纯基础 RPC

**Files:**
- Modify: `include/apps/minirpc/common/minirpc_types.h`
- Modify: `include/apps/minirpc/common/minirpc_codec.h`
- Modify: `include/apps/minirpc/parent/minirpc_async_client.h`
- Modify: `include/apps/minirpc/parent/minirpc_client.h`
- Modify: `include/apps/minirpc/child/minirpc_service.h`
- Modify: `src/apps/minirpc/common/minirpc_codec.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_client.cpp`
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `demo/minirpc_demo_main.cpp`
- Modify: `tests/minirpc_codec_test.cpp`
- Modify: `tests/minirpc_client_test.cpp`
- Modify: `tests/minirpc_service_test.cpp`

**Step 1: Write the failing test**

调整测试期望：

- `MiniRpc` 只保留 `Echo / Add / Sleep`
- 不再存在 `Notify` request/reply/回调

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_codec_test|memrpc_minirpc_service_test|memrpc_minirpc_client_test'`

Expected:

- FAIL，当前 `MiniRpc` 仍依赖 notify 代码

**Step 3: Write minimal implementation**

删除 `MiniRpc` 中的：

- `NotifyRequest`
- `NotifyEvent`
- `PollNotifyReply`
- `NotifyAsync()` / `Notify()`
- service 侧通知发布逻辑

保留：

- `Echo`
- `Add`
- `Sleep`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_codec_test|memrpc_minirpc_service_test|memrpc_minirpc_client_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc src/apps/minirpc demo/minirpc_demo_main.cpp tests/minirpc_codec_test.cpp tests/minirpc_client_test.cpp tests/minirpc_service_test.cpp
git commit -m "refactor: simplify minirpc to basic request response"
```

### Task 4: 在 VPS 子进程补齐 `PollBehaviorReports()` RPC

**Files:**
- Modify: `include/apps/vps/common/virus_protection_service_define.h`
- Modify: `include/apps/vps/common/vps_codec.h`
- Modify: `include/apps/vps/child/virus_engine_service.h`
- Modify: `src/apps/vps/common/vps_codec.cpp`
- Modify: `src/apps/vps/child/virus_engine_service.cpp`
- Modify: `src/core/protocol.h`
- Modify: `tests/vps_codec_test.cpp`
- Modify: `tests/vps_service_test.cpp`

**Step 1: Write the failing test**

补两类测试：

- `PollBehaviorReportsReply` codec round-trip
- `ScanBehavior` 触发后，`PollBehaviorReports()` 能批量拉出待上报结果

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_codec_test|memrpc_vps_service_test'`

Expected:

- FAIL，缺少新的 reply 结构、codec 或 handler

**Step 3: Write minimal implementation**

实现：

- `PollBehaviorReportsReply`
- `Encode/DecodePollBehaviorReportsReply`
- `Opcode::VpsPollBehaviorReports`
- service 内部队列批量出队

第一版请求可为空，默认批量大小使用小常量。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_codec_test|memrpc_vps_service_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/vps/common/virus_protection_service_define.h include/apps/vps/common/vps_codec.h include/apps/vps/child/virus_engine_service.h src/apps/vps/common/vps_codec.cpp src/apps/vps/child/virus_engine_service.cpp src/core/protocol.h tests/vps_codec_test.cpp tests/vps_service_test.cpp
git commit -m "feat: add polling rpc for behavior reports"
```

### Task 5: 在 VPS 主进程补轮询线程和本地 listener 广播

**Files:**
- Modify: `include/apps/vps/parent/virus_engine_manager.h`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`
- Modify: `tests/vps_manager_integration_test.cpp`
- Modify: `tests/vps_listener_polling_test.cpp`

**Step 1: Write the failing test**

覆盖以下行为：

- 没有 listener 时不启动轮询线程
- 注册第一个 listener 时启动轮询线程
- `ScanBehavior` 后能通过轮询拿到 `BehaviorScanResult`
- 本地 listener 能收到广播
- 注销最后一个 listener 后线程停止

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_manager_integration_test|memrpc_vps_listener_polling_test'`

Expected:

- FAIL，当前 manager 尚未用轮询线程驱动广播

**Step 3: Write minimal implementation**

在主进程 manager 中新增最小轮询逻辑：

- listener 数量 `0 -> 1` 时拉起线程
- 线程周期性调用 `PollBehaviorReports()`
- 对返回的 `reports` 逐条广播
- listener 数量降为 `0` 时退出线程

轮询线程仅依赖普通 RPC，不依赖 notify API。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_vps_manager_integration_test|memrpc_vps_listener_polling_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/vps/parent/virus_engine_manager.h src/apps/vps/parent/virus_engine_manager.cpp tests/vps_manager_integration_test.cpp tests/vps_listener_polling_test.cpp
git commit -m "feat: poll behavior reports in vps manager"
```

### Task 6: 回归核心 RPC 路径并更新文档

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`
- Modify: `docs/plans/2026-03-09-minirpc-first-framework-design.md`
- Modify: `docs/plans/2026-03-09-minirpc-first-framework-implementation-plan.md`
- Modify: `demo/demo_dual_process_main.cpp`

**Step 1: Write the failing test**

使用现有回归与 demo 作为护栏：

- dual-process demo
- end-to-end integration
- minirpc demo

并检查文档不再描述 notify 通道主线。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_integration_end_to_end_test|memrpc_vps_manager_integration_test|memrpc_minirpc_client_test'`

Expected:

- FAIL，直到代码和文档都收敛到 polling 方案

**Step 3: Write minimal implementation**

更新：

- `architecture.md`
- `demo_guide.md`
- `porting_guide.md`
- `minirpc-first` 设计和计划文档

确保文档表述统一为：

- 框架主线只有 request/response
- listener 场景通过 `Poll...()` RPC 实现

**Step 4: Run full verification**

Run: `cmake --build build`

Expected:

- BUILD PASS

Run: `ctest --test-dir build --output-on-failure`

Expected:

- 全部 PASS

Run: `./build/demo/memrpc_demo_dual_process`

Expected:

- 正常输出 `normal/high/timeout`

Run: `./build/demo/memrpc_minirpc_demo`

Expected:

- 正常输出 `echo/add/sleep`

**Step 5: Commit**

```bash
git add docs demo/demo_dual_process_main.cpp
git commit -m "docs: align polling-based behavior reporting"
```
