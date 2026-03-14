# VES DFX And Recovery Closure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 Virus Executor Service 的 DFX / 心跳 / 恢复链路收口成统一、清晰、可复用的设计：heartbeat 协议保留在 bootstrap/proxy 层，heartbeat 调度由 `MemRpc::RpcClient` 的 watchdog 驱动，`MemRpc` 只消费通用健康结果，上层 client 默认不参与主恢复闭环，只保留可选扩展 snapshot 订阅能力。

**Architecture:** 采用“协议下沉、调度上收、恢复收口、观测旁路”的分层设计。`VesControlProxy` 负责实现 VES-specific heartbeat RPC 并把 reply 翻译成通用健康结果；`IBootstrapChannel` 对外暴露 `CheckHealth()` 这类通用接口；`MemRpc::RpcClient` 在自己的 watchdog 中周期性检查健康、统一执行 restart/close；`VesClient` 只负责业务默认参数、业务日志、可选 snapshot 订阅。supervisor / harness 只负责 process respawn，不参与 session replay。

**Tech Stack:** C++17, shared memory, eventfd, Unix-domain socket, `MemRpc::RecoveryPolicy`, OHOS SA mock transport, GoogleTest, DT/stress/integration tests

---

## 1. Background

当前代码已经有这些能力：

- `RpcClient` 有 watchdog、timeout scan、idle 处理、forced restart、safe replay。
- `VesControlProxy` 已支持 `Heartbeat()` RPC，也有 control socket death monitor。
- `VirusExecutorService::Heartbeat()` 已返回基础业务健康快照。
- `VesClient` 已配置 `onFailure / onIdle / onEngineDeath`。

但现在仍有三个主要问题：

1. heartbeat 没进入统一恢复主链路。
2. heartbeat、engine death、idle、exec-timeout 的 owner 仍然不清晰。
3. heartbeat 调度放哪层、业务 reply 是否进入 `memrpc`，还没有定型。

`problem1` 默认已修复，本计划不再针对那个 bug；本轮关注架构收口和可维护性。

## 2. Core Problem

现状里最容易乱的点：

1. heartbeat 发现故障后，是否应直接 `CloseSession()`，语义不清。
2. 如果 heartbeat loop 放在上层 `VesClient`，会和 `RpcClient` watchdog、`EngineDeathCallback`、proxy monitor 形成多 owner。
3. 如果让 `memrpc` 直接解析 `VesHeartbeatReply`，会把业务协议污染进框架 core。
4. 如果把 heartbeat 调度完全放到 proxy 自己线程里，又会和 `RpcClient` 的 watchdog 节拍分裂。

## 3. Design Goals

完成后应达到：

1. heartbeat 协议留在 bootstrap/proxy，`memrpc` 不依赖业务 heartbeat reply 结构。
2. `RpcClient` 的 watchdog 成为统一健康检查调度者。
3. `RpcClient` 成为唯一 session 恢复执行者。
4. 上层 `VesClient` 默认不需要参与 heartbeat 恢复闭环。
5. 上层若要做 DFX 展示，可以订阅 VES-specific snapshot。
6. `CloseSession` 与 `Restart` 的语义完全分开：
   - `CloseSession` 只用于 intentional close
   - 故障恢复统一走 `Restart`

## 4. Non-Goals

- 不重新设计 shared-memory wire protocol
- 不引入多 client / 多 session
- 不实现 hard kill handler
- 不把 `VesHeartbeatReply` 引入 `memrpc` core
- 不在本轮处理所有性能问题

## 5. Chosen Design

### 5.1 Layering

最终分层：

- `VesControlProxy`
  - 实现 VES heartbeat 请求/应答协议
  - 解析 `VesHeartbeatReply`
  - 把业务 reply 翻译成通用 `ChannelHealthResult`
  - 可选对外发布 VES-specific snapshot

- `IBootstrapChannel`
  - 暴露通用健康检查接口
  - 不暴露业务 heartbeat 结构

- `MemRpc::RpcClient`
  - 在 watchdog 中调用 `CheckHealth()`
  - 根据通用健康结果发起统一恢复
  - 维护 cooldown / replay / poison-pill 语义

- `VesClient`
  - 默认策略配置
  - 业务 facade
  - 可选 health snapshot 订阅
  - 业务日志与 DFX 展示

- supervisor / harness
  - engine process respawn
  - `waitpid/reap`
  - process 生命周期

### 5.2 Single Recovery Owner

唯一 session 恢复 owner：

- `MemRpc::RpcClient`

含义：

- heartbeat 不直接 `CloseSession()`
- heartbeat 不 fake `EngineDeathCallback`
- proxy 不直接决定 restart
- `VesClient` 不直接执行恢复动作

### 5.3 Protocol Down, Scheduling Up

这是本版设计的核心：

- **协议下沉**
  - VES-specific heartbeat request/reply 仍在 `VesControlProxy`
- **调度上收**
  - heartbeat 的周期性检查由 `RpcClient` watchdog 发起

这样能同时满足：

- 不增加额外长期 heartbeat 线程
- 复用 `RpcClient` 既有 watchdog 节拍
- 把 timeout/idle/health check 放在统一 owner 下

### 5.4 Generic Health Check Interface

`memrpc` 不应该知道 `VesHeartbeatReply`。  
因此 `IBootstrapChannel` 应新增一个通用健康检查接口，例如：

```cpp
enum class ChannelHealthStatus {
  Healthy,
  Timeout,
  Malformed,
  Unhealthy,
  SessionMismatch,
  Unsupported,
};

struct ChannelHealthResult {
  ChannelHealthStatus status = ChannelHealthStatus::Unsupported;
  uint64_t sessionId = 0;
};

class IBootstrapChannel {
 public:
  virtual ChannelHealthResult CheckHealth(uint64_t expectedSessionId) = 0;
};
```

这个接口的职责：

- 只返回通用健康结论
- 不返回业务扩展数据
- 允许不支持 heartbeat 的 bootstrap 返回 `Unsupported`

### 5.5 Where The Business Reply Lives

业务 heartbeat reply 仍然存在，但只停留在 VES transport 层。

必须写死：

- `VesControlProxy` 可以直接收发 `VesHeartbeatReply`
- `VesControlProxy::CheckHealth()` 内部可以解析它
- 但 `VesHeartbeatReply` 不能进入 `memrpc` core

也就是说，数据流必须是：

- **恢复链路**
  - `VesHeartbeatReply -> proxy translation -> ChannelHealthResult -> RpcClient`

- **观测链路**
  - `VesHeartbeatReply -> optional snapshot subscriber -> VesClient / DFX`

绝不允许：

- `VesHeartbeatReply -> memrpc core`
- `memrpc` include `ves_control_interface.h`

### 5.6 Optional Snapshot Subscription

恢复链路和观测链路必须分离。

恢复链路只依赖：

- `ChannelHealthResult`

观测链路可以依赖：

- `VesHeartbeatReply`
- 或者一个 VES-specific health snapshot wrapper

可以接受的实现方式：

- `VesControlProxy` 提供 snapshot callback
- `VesClient` 透传订阅接口

必须明确：

- snapshot callback 是 side-channel
- side-channel 失败不能影响主恢复链路
- 没有 snapshot subscriber 时，heartbeat 恢复仍必须正常工作

### 5.7 `RpcClient` External Recovery Entry

为了让通用健康结果进入统一恢复骨架，需要给 `RpcClient` 增加外部恢复入口。

推荐 API：

```cpp
enum class ExternalRecoverySignal {
  ChannelHealthTimeout,
  ChannelHealthMalformed,
  ChannelHealthUnhealthy,
  ChannelHealthSessionMismatch,
};

struct ExternalRecoveryRequest {
  ExternalRecoverySignal signal;
  uint64_t sessionId = 0;
  uint32_t delayMs = 0;
};

class RpcClient {
 public:
  void RequestExternalRecovery(ExternalRecoveryRequest request);
};
```

设计要求：

- 只负责导入外部故障信号
- 内部最终复用 `RequestForcedRestart()`
- 保留 cooldown / replay / restart gating
- 不伪造 engine death report

### 5.8 Watchdog Integration

`RpcClient` watchdog 未来需要做三件事：

1. timeout scan
2. idle handling
3. channel health check

推荐顺序：

1. scan pending timeouts
2. check channel health
3. handle idle reminder

注意：

- `CheckHealth()` 必须有短超时，不能卡住 watchdog
- 调用 `CheckHealth()` 时不能持有会与 reopen/close 冲突的重锁

### 5.9 Explicit Semantics

必须明确：

- `CloseSession`
  - 仅用于 intentional close
  - 场景：idle unload、manual shutdown

- `Restart`
  - 用于故障恢复
  - 场景：heartbeat failure、engine death、exec-timeout

- `EngineDeath`
  - 是 signal / reason，不是动作

- `HeartbeatFailure`
  - 也是 signal / reason，不是动作

### 5.10 Recovery Reason Model

统一 reason：

- `Unknown`
- `IdleThresholdReached`
- `HeartbeatTimeout`
- `HeartbeatMalformed`
- `HeartbeatUnhealthy`
- `HeartbeatSessionMismatch`
- `EngineDeath`
- `ExecTimeout`
- `QueueTimeout`
- `ProtocolMismatch`
- `ManualClose`

### 5.11 VES Health Snapshot Model

heartbeat reply 仍由 VES 层定义。  
推荐增强字段：

- `version`
- `status`
- `reasonCode`
- `sessionId`
- `inFlight`
- `lastTaskAgeMs`
- `currentTask`
- `flags`

服务端健康状态建议：

- `OkIdle`
- `OkBusy`
- `DegradedLongRunning`
- `UnhealthyNoSession`
- `UnhealthyStopping`
- `UnhealthyInternalError`

### 5.12 Process Supervision Boundary

必须写死：

- session 恢复：`RpcClient`
- process respawn：supervisor / DT / stress harness

`RpcClient` 不负责 fork/exec engine。  
supervisor 不负责 replay-safe 请求恢复。

## 6. Execution Order

建议顺序：

1. 先冻结语义测试
2. 再加 `IBootstrapChannel::CheckHealth()`
3. 再加 `RpcClient::RequestExternalRecovery()`
4. 再把 watchdog 接到 `CheckHealth()`
5. 再补 snapshot side-channel
6. 再丰富 heartbeat snapshot
7. 最后统一 supervisor / harness 和文档

---

### Task 1: Freeze The New Layering And Semantics In Tests

**Files:**
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `memrpc/tests/rpc_client_idle_callback_test.cpp`
- Create: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing tests**

锁死以下语义：

- heartbeat failure 触发 `Restart`
- idle 触发 `CloseSession`
- heartbeat failure 不等于 engine death
- `VesClient` 不需要自己维护 heartbeat loop 才能恢复
- snapshot 订阅是可选 side-channel

**Step 2: Run tests to verify they fail**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

Expected:

- FAIL，因为当前分层和语义还没收口

**Step 3: Add minimal test scaffolding**

先补最小脚手架，保证后续可以逐步推进。

**Step 4: Re-run tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

Expected:

- 编译通过
- 行为断言仍失败

**Step 5: Commit**

```bash
git add virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        memrpc/tests/rpc_client_idle_callback_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "test: freeze ves dfx layering semantics"
```

### Task 2: Add Generic `CheckHealth()` To `IBootstrapChannel`

**Files:**
- Modify: `memrpc/include/memrpc/core/bootstrap.h`
- Modify: `memrpc/include/memrpc/client/dev_bootstrap.h`
- Modify: `memrpc/src/bootstrap/dev_bootstrap.cpp`
- Modify: `memrpc/include/memrpc/client/sa_bootstrap.h`
- Modify: `memrpc/src/bootstrap/sa_bootstrap.cpp`
- Modify: `virus_executor_service/include/transport/ves_control_proxy.h`
- Modify: `virus_executor_service/src/transport/ves_control_proxy.cpp`
- Create: `memrpc/tests/bootstrap_health_check_test.cpp`
- Modify: `memrpc/tests/CMakeLists.txt`

**Step 1: Write the failing test**

补测试覆盖：

- `IBootstrapChannel::CheckHealth()` 存在
- 对不支持 heartbeat 的 bootstrap 返回 `Unsupported`
- `VesControlProxy` 能把业务 heartbeat reply 翻译成通用 `ChannelHealthResult`
- `memrpc` test 不依赖 `VesHeartbeatReply`

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'bootstrap_health_check'
```

Expected:

- FAIL，因为当前没有 `CheckHealth()`

**Step 3: Write minimal implementation**

新增：

- `ChannelHealthStatus`
- `ChannelHealthResult`
- `IBootstrapChannel::CheckHealth(uint64_t expectedSessionId)`

要求：

- `memrpc` core 只看到 generic result
- VES-specific reply 只停留在 proxy 内部

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'bootstrap_health_check'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/core/bootstrap.h \
        memrpc/include/memrpc/client/dev_bootstrap.h \
        memrpc/src/bootstrap/dev_bootstrap.cpp \
        memrpc/include/memrpc/client/sa_bootstrap.h \
        memrpc/src/bootstrap/sa_bootstrap.cpp \
        virus_executor_service/include/transport/ves_control_proxy.h \
        virus_executor_service/src/transport/ves_control_proxy.cpp \
        memrpc/tests/bootstrap_health_check_test.cpp \
        memrpc/tests/CMakeLists.txt
git commit -m "feat: add bootstrap channel health check api"
```

### Task 3: Add `RpcClient::RequestExternalRecovery()`

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Create: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Modify: `memrpc/tests/engine_death_handler_test.cpp`
- Modify: `memrpc/tests/CMakeLists.txt`

**Step 1: Write the failing test**

补测试覆盖：

- `RequestExternalRecovery()` 存在
- 它复用 forced restart 流程
- cooldown / gating 生效
- 不伪造 engine death

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_external_recovery|engine_death_handler'
```

Expected:

- FAIL，因为当前没有外部恢复入口

**Step 3: Write minimal implementation**

新增：

- `ExternalRecoverySignal`
- `ExternalRecoveryRequest`
- `RequestExternalRecovery()`

要求：

- 只导入外部故障信号
- 内部仍复用 `RequestForcedRestart()`

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_external_recovery|engine_death_handler'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h \
        memrpc/src/client/rpc_client.cpp \
        memrpc/tests/rpc_client_external_recovery_test.cpp \
        memrpc/tests/engine_death_handler_test.cpp \
        memrpc/tests/CMakeLists.txt
git commit -m "feat: add rpc client external recovery entry"
```

### Task 4: Integrate `CheckHealth()` Into `RpcClient` Watchdog

**Files:**
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `memrpc/tests/rpc_client_idle_callback_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- watchdog 会周期性调用 `CheckHealth()`
- `Healthy` 不触发恢复
- `Timeout/Malformed/Unhealthy/SessionMismatch` 会走 `RequestExternalRecovery()`
- `Unsupported` 不影响现有 bootstrap

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|rpc_client_idle_callback|ves_policy'
```

Expected:

- FAIL，因为当前 watchdog 还没有健康检查

**Step 3: Write minimal implementation**

要求：

- `WatchdogLoop()` 中接入 `CheckHealth()`
- 不让 `CheckHealth()` 持锁阻塞恢复路径
- 映射成 `RequestExternalRecovery()`
- `VesClient` 不增加 heartbeat loop

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|rpc_client_idle_callback|ves_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/src/client/rpc_client.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        memrpc/tests/rpc_client_idle_callback_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp
git commit -m "feat: integrate channel health checks into rpc watchdog"
```

### Task 5: Add Optional VES Snapshot Subscription

**Files:**
- Modify: `virus_executor_service/include/transport/ves_control_proxy.h`
- Modify: `virus_executor_service/src/transport/ves_control_proxy.cpp`
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Create: `virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing test**

补测试覆盖：

- 上层可订阅完整 VES health snapshot
- 没有订阅时不影响恢复主链路
- snapshot callback 的异常/阻塞不会阻断恢复主链路

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health_subscription'
```

Expected:

- FAIL，因为当前没有 side-channel snapshot subscription

**Step 3: Write minimal implementation**

实现策略：

- `VesControlProxy` 提供 VES-specific snapshot callback
- `VesClient` 透传订阅接口
- 保持 snapshot 订阅完全独立于主恢复链路

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health_subscription'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/transport/ves_control_proxy.h \
        virus_executor_service/src/transport/ves_control_proxy.cpp \
        virus_executor_service/include/client/ves_client.h \
        virus_executor_service/src/client/ves_client.cpp \
        virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "feat: add optional ves heartbeat snapshot subscription"
```

### Task 6: Enrich VES Health Snapshot And DFX Reasons

**Files:**
- Modify: `virus_executor_service/include/transport/ves_control_interface.h`
- Modify: `virus_executor_service/src/service/virus_executor_service.cpp`
- Modify: `virus_executor_service/include/ves/ves_engine_service.h`
- Modify: `virus_executor_service/src/service/ves_engine_service.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_health_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `OkIdle`
- `OkBusy`
- `DegradedLongRunning`
- `UnhealthyNoSession`
- `reasonCode` 与状态一致

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health|ves_heartbeat|ves_recovery_reason'
```

Expected:

- FAIL，因为当前 snapshot 还不够细

**Step 3: Write minimal implementation**

增强 VES heartbeat reply：

- `reasonCode`
- `flags`
- 更清晰的 health state helper

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health|ves_heartbeat|ves_recovery_reason'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/transport/ves_control_interface.h \
        virus_executor_service/src/service/virus_executor_service.cpp \
        virus_executor_service/include/ves/ves_engine_service.h \
        virus_executor_service/src/service/ves_engine_service.cpp \
        virus_executor_service/tests/unit/ves/ves_health_test.cpp \
        virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp
git commit -m "feat: enrich ves heartbeat health snapshot and reasons"
```

### Task 7: Improve ExecTimeout DFX Fidelity

**Files:**
- Modify: `memrpc/src/server/rpc_server.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 长执行期间 runtime heartbeat 推进
- timeout 仍归因为 `ExecTimeout`
- 不被误记为 heartbeat failure

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|ves_policy'
```

Expected:

- FAIL，因为当前 long-running DFX fidelity 仍不够

**Step 3: Write minimal implementation**

要求：

- 周期性更新 `lastHeartbeatMonoMs`
- 只增强 DFX，不改 soft timeout 语义

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|ves_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/src/server/rpc_server.cpp \
        memrpc/src/client/rpc_client.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp
git commit -m "feat: improve exec-timeout dfx fidelity"
```

### Task 8: Unify Process Respawn Ownership In Supervisor And Harnesses

**Files:**
- Modify: `virus_executor_service/src/app/ves_supervisor_main.cpp`
- Modify: `virus_executor_service/src/app/ves_dt_crash_recovery_main.cpp`
- Modify: `virus_executor_service/src/app/ves_stress_client_main.cpp`
- Create: `virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp`
- Create: `virus_executor_service/tests/dt/ves_idle_reopen_test.cpp`
- Modify: `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing test**

补 DT 覆盖：

- heartbeat failure -> process respawn -> session reopen
- idle close -> next call reopen
- heartbeat failure 与 engine death overlap 不会双重恢复
- supervisor 能 runtime reap/respawn

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|ves_heartbeat_recovery|ves_idle_reopen|virus_executor_service_supervisor_integration'
```

Expected:

- FAIL，因为 process respawn owner 还不统一

**Step 3: Write minimal implementation**

要求：

- supervisor 负责 process reap/respawn
- `RpcClient` 负责 session 恢复
- 日志必须区分 process respawn 与 session replay

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|ves_heartbeat_recovery|ves_idle_reopen|virus_executor_service_supervisor_integration'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/src/app/ves_supervisor_main.cpp \
        virus_executor_service/src/app/ves_dt_crash_recovery_main.cpp \
        virus_executor_service/src/app/ves_stress_client_main.cpp \
        virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp \
        virus_executor_service/tests/dt/ves_idle_reopen_test.cpp \
        virus_executor_service/tests/dt/ves_crash_recovery_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "feat: unify ves process respawn ownership"
```

### Task 9: Align Docs And Gates

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`
- Modify: `docs/demo_guide.md`
- Modify: `tools/push_gate.sh`
- Modify: `tools/ci_sweep.sh`
- Create: `docs/plans/2026-03-14-ves-dfx-recovery-closure.md`

**Step 1: Write the mismatch checklist**

确认必须更新：

- heartbeat 调度由 `RpcClient` watchdog 驱动
- heartbeat 协议仍在 proxy
- `memrpc` 不依赖业务 heartbeat reply
- snapshot 订阅是 side-channel

**Step 2: Verify the mismatch**

Run:

```bash
rg -n "heartbeat|CheckHealth|CloseSession|Restart|replay|engine death" docs tools memrpc virus_executor_service
```

Expected:

- 能定位不一致点

**Step 3: Write minimal updates**

文档明确：

- protocol down, scheduling up
- `IBootstrapChannel::CheckHealth()`
- `RpcClient` 统一健康检查与恢复
- process respawn 不属于 `RpcClient`

并把新的 DT case 纳入 gate。

**Step 4: Re-scan for consistency**

Run:

```bash
rg -n "CheckHealth|RequestExternalRecovery|Heartbeat|CloseSession|Restart" docs tools
```

Expected:

- 关键术语一致

**Step 5: Commit**

```bash
git add docs/architecture.md \
        docs/sa_integration.md \
        docs/demo_guide.md \
        tools/push_gate.sh \
        tools/ci_sweep.sh \
        docs/plans/2026-03-14-ves-dfx-recovery-closure.md
git commit -m "docs: align ves dfx recovery layering"
```

---

## 7. Verification Matrix

阶段验证：

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_health|ves_policy|ves_recovery_reason|ves_health_subscription|bootstrap_health_check|rpc_client_external_recovery|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

DT 验证：

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|ves_heartbeat_recovery|ves_idle_reopen|virus_executor_service_supervisor_integration'
```

最终验证：

```bash
tools/push_gate.sh --deep
```

## 8. Risks

1. `CheckHealth()` 如果超时控制不好，会卡住 watchdog。
2. control socket death monitor 与健康检查可能重复触发恢复。
3. 如果把业务 snapshot 误放进 `memrpc` 通用接口，会破坏分层。
4. process respawn 与 session replay 仍可能被错误实现成双重恢复。

## 9. Mitigations

- `CheckHealth()` 必须有短超时和明确返回语义。
- `RpcClient::RequestExternalRecovery()` 做统一 de-dup/gating。
- snapshot 订阅保持 VES-specific，不进入 `memrpc` core。
- 代码审查中检查是否有任何 `memrpc` 文件直接依赖 VES heartbeat 定义。

## 10. Recommended First Slice

优先顺序：

1. Task 1
2. Task 2
3. Task 3
4. Task 4

这四步先把最关键的主链路收口：

- heartbeat 协议留在 proxy
- 健康检查统一从 `IBootstrapChannel::CheckHealth()` 暴露
- watchdog 统一驱动健康检查
- `RpcClient` 成为单一恢复执行者
