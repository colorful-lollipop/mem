# VES DFX And Recovery Closure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 Virus Executor Service 的 DFX / 心跳 / 恢复链路收口成单一、清晰、可诊断的设计，让故障检测、恢复决策、恢复动作、进程监督各自职责明确。

**Architecture:** 采用“单一恢复 owner”设计。`MemRpc::RpcClient` 是唯一 session 恢复执行者；`VesClient` 负责汇聚 heartbeat / idle / engine death / timeout 等信号并生成统一恢复请求；`VesControlProxy` 只负责 transport 级 liveness 输入；supervisor / harness 只负责进程 respawn，不负责 session replay。heartbeat 不再直接 `CloseSession()`，而是作为健康信号进入统一恢复入口。

**Tech Stack:** C++17, shared memory, eventfd, Unix-domain socket, `MemRpc::RecoveryPolicy`, OHOS SA mock transport, GoogleTest, DT/stress/integration tests

---

## 1. Background

当前代码里已经有不少恢复相关零件，但语义边界不够清楚：

- `RpcClient` 已有 `RecoveryPolicy`、forced restart、session reopen、safe replay、poison-pill fail。
- `VirusExecutorService::Heartbeat()` 已提供基础健康快照。
- `VesControlProxy` 已支持 heartbeat 请求，也能通过 control socket 断开感知 engine death。
- `VesClient` 目前只接了 `onFailure / onIdle / onEngineDeath`，heartbeat 还没有真正接进运行时恢复闭环。
- `ExecTimeout` 目前仍是软超时，能触发客户端失败，但不等于“服务端 hang 已被治理”。
- supervisor、DT harness、stress harness 在 crash respawn 上各有一套做法。

`problem1` 已默认修复，本计划不再处理它；本轮重点是把现有能力打磨成可长期维护的 DFX / 恢复体系。

## 2. Core Problem

现在最混乱的点不是“有没有功能”，而是“谁负责什么”不够统一：

1. heartbeat 发现问题后，到底应该直接 `CloseSession()`，还是走 callback/recovery policy，不清楚。
2. `CloseSession` 和 `Restart` 的语义没有被明确区分。
3. `engine death`、`heartbeat unhealthy`、`exec-timeout`、`idle close` 都会影响恢复，但没有统一 reason 模型。
4. transport、framework、client facade、supervisor 都在碰恢复，容易双重触发。
5. 文档和代码已经发生漂移，后续实现容易继续变乱。

## 3. Design Goals

完成后需要达到：

1. 故障检测、恢复决策、恢复动作、进程监督四层职责清晰。
2. heartbeat 进入统一恢复通道，不再自己偷偷做恢复动作。
3. `CloseSession` 只保留“有意关闭”语义；故障恢复统一走 `Restart`。
4. 所有恢复动作都能带着明确的 `reason` 被记录、测试和解释。
5. DFX 日志能回答三个问题：
   - 为什么恢复？
   - 谁发起恢复？
   - 恢复做到了哪一步？

## 4. Non-Goals

- 不重新设计 shared-memory wire protocol
- 不引入多 client / 多 session
- 不实现真正 hard kill handler
- 不做复杂 metrics/dashboard 平台接入
- 不在本轮解决所有性能问题

## 5. Chosen Design

### 5.1 Single Recovery Owner

选定设计：

- `RpcClient`：唯一恢复执行者
- `VesClient`：唯一恢复请求汇聚者
- `VesControlProxy`：只负责检测 transport 级信号
- supervisor / harness：只负责进程 respawn

含义是：

- heartbeat 不直接调用恢复动作
- proxy 不直接决定恢复策略
- `RpcClient` 不直接知道 heartbeat 细节，只接收一个统一的“外部恢复请求”

### 5.2 Signal / Decision / Action Split

统一分成三层：

1. **Signal**
   - heartbeat timeout
   - heartbeat unhealthy
   - heartbeat session mismatch
   - engine death
   - exec-timeout
   - idle threshold reached

2. **Decision**
   - Ignore
   - Restart
   - CloseSession

3. **Action**
   - `RequestForcedRestart(delay)`
   - `RequestSessionClose()`
   - no-op

### 5.3 Explicit Semantics

必须写死这几个语义：

- `CloseSession`
  - 只用于有意关闭
  - 典型场景：idle unload、manual shutdown
  - 目标：让服务退出、释放资源

- `Restart`
  - 用于故障恢复
  - 典型场景：heartbeat failure、engine death、exec-timeout
  - 目标：`CloseSession -> cooldown -> OpenSession -> replay safe calls`

- `EngineDeath`
  - 只是一个 signal / reason，不是一个动作

- `HeartbeatFailure`
  - 也是 signal / reason，不是动作

### 5.4 Heartbeat Best Design

heartbeat 的最佳设计是：

- `VesClient` 内部维护 heartbeat loop
- heartbeat loop 周期性调用 `VesControlProxy::Heartbeat()`
- 如果出现以下情况：
  - `PeerDisconnected`
  - `ProtocolMismatch`
  - reply version invalid
  - `reply.status != OK`
  - `reply.sessionId != currentSessionId`
- `VesClient` 只做两件事：
  - 记录恢复 reason
  - 向 `RpcClient` 提交统一恢复请求

heartbeat 不做：

- 不直接 `proxy_->CloseSession()`
- 不手工伪造 `EngineDeathCallback`
- 不在 proxy 层硬编码 restart policy

### 5.5 New API Surface

为避免 heartbeat 只能“伪装成 engine death”，本计划引入新的统一入口。

推荐 API：

```cpp
enum class ExternalRecoverySignal {
  HeartbeatTimeout,
  HeartbeatMalformed,
  HeartbeatUnhealthy,
  HeartbeatSessionMismatch,
};

struct ExternalRecoveryRequest {
  ExternalRecoverySignal signal;
  uint32_t delayMs = 0;
};

class RpcClient {
 public:
  void RequestExternalRecovery(ExternalRecoveryRequest request);
};
```

设计原则：

- `RequestExternalRecovery()` 只负责把外部健康失败导入已有恢复骨架
- 内部最终仍复用 `RequestForcedRestart()`
- gating / cooldown / replay / poison-pill 逻辑全部保留在 `RpcClient`

### 5.6 Recovery Reason Model

新增统一 reason：

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

reason 用于：

- DFX 日志
- 对外 debug 接口
- 单元测试断言
- DT/stress 回归验证

### 5.7 Health Snapshot Model

heartbeat reply 需要从“简单状态”提升到“可解释状态”。

推荐最终模型：

- `version`
- `status`
- `reasonCode`
- `sessionId`
- `inFlight`
- `lastTaskAgeMs`
- `currentTask`
- `flags`

如果 wire 兼容必须保守，至少增加 `reasonCode`。

服务端状态建议：

- `OkIdle`
- `OkBusy`
- `DegradedLongRunning`
- `UnhealthyNoSession`
- `UnhealthyStopping`
- `UnhealthyInternalError`

### 5.8 Process Supervision Boundary

明确边界：

- `RpcClient`
  - session 级恢复
  - replay-safe 请求恢复
- `VesClient`
  - heartbeat loop
  - reason 归因
  - 发起外部恢复请求
- `VesControlProxy`
  - 仅上报 transport death
- supervisor / DT / stress harness
  - 进程 respawn
  - `waitpid/reap`
  - engine process 生命周期

process respawn 不进入 `RpcClient`。

## 6. Implementation Strategy

顺序必须是：

1. 先定语义和测试
2. 再补 `RpcClient::RequestExternalRecovery`
3. 再接 `VesClient` heartbeat runtime
4. 再丰富 health snapshot
5. 再统一 supervisor / harness
6. 最后更新文档和 gate

---

### Task 1: Freeze The New Recovery Semantics In Tests

**Files:**
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `memrpc/tests/rpc_client_idle_callback_test.cpp`
- Create: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing tests**

先写语义测试，锁死以下规则：

- idle 触发 `CloseSession`
- engine death 触发 `Restart`
- heartbeat timeout / unhealthy / session mismatch 触发 `Restart`
- heartbeat failure 不会被归类成 engine death
- `ExecTimeout` 与 `HeartbeatTimeout` reason 不同

至少包含：

```cpp
TEST(VesRecoveryReasonTest, HeartbeatTimeoutRequestsRestart);
TEST(VesRecoveryReasonTest, HeartbeatUnhealthyRequestsRestart);
TEST(VesRecoveryReasonTest, IdleRequestsCloseSession);
TEST(VesRecoveryReasonTest, EngineDeathReasonStaysDistinct);
```

**Step 2: Run tests to verify they fail**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

Expected:

- FAIL，因为当前还没有统一 recovery reason 和外部恢复请求入口

**Step 3: Add minimal test scaffolding**

只补最小编译脚手架：

- reason enum 占位
- 访问接口占位
- test seam

**Step 4: Run tests again**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

Expected:

- 编译通过
- 语义断言仍失败

**Step 5: Commit**

```bash
git add virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        memrpc/tests/rpc_client_idle_callback_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "test: freeze ves recovery semantics"
```

### Task 2: Add A Unified External Recovery Entry To `RpcClient`

**Files:**
- Modify: `memrpc/include/memrpc/client/rpc_client.h`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/tests/engine_death_handler_test.cpp`
- Modify: `memrpc/tests/rpc_client_recovery_policy_test.cpp`
- Create: `memrpc/tests/rpc_client_external_recovery_test.cpp`
- Modify: `memrpc/tests/CMakeLists.txt`

**Step 1: Write the failing test**

补测试覆盖：

- `RequestExternalRecovery()` 存在
- 它会复用 forced restart 路径
- 如果 restart 已在进行，新的外部恢复请求会被 gate
- 外部恢复不会伪造 engine death report

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_external_recovery|engine_death_handler|rpc_client_recovery_policy'
```

Expected:

- FAIL，因为当前没有 `RequestExternalRecovery()`

**Step 3: Write minimal implementation**

新增：

- `ExternalRecoverySignal`
- `ExternalRecoveryRequest`
- `RpcClient::RequestExternalRecovery()`

实现要求：

- 只做 signal 导入
- 内部统一落到 `RequestForcedRestart()`
- 保留现有 cooldown / replay / restart_pending 逻辑

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_external_recovery|engine_death_handler|rpc_client_recovery_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/client/rpc_client.h \
        memrpc/src/client/rpc_client.cpp \
        memrpc/tests/engine_death_handler_test.cpp \
        memrpc/tests/rpc_client_recovery_policy_test.cpp \
        memrpc/tests/rpc_client_external_recovery_test.cpp \
        memrpc/tests/CMakeLists.txt
git commit -m "feat: add rpc client external recovery entry"
```

### Task 3: Introduce A First-Class VES Recovery Reason Model

**Files:**
- Create: `virus_executor_service/include/client/ves_dfx_types.h`
- Create: `virus_executor_service/src/client/ves_dfx_types.cpp`
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `VesClient` 能记录最近恢复 reason
- 不同 signal 会记录不同 reason
- `LastRecoveryReason()` 可读

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_recovery_reason|ves_policy'
```

Expected:

- FAIL，因为 `VesRecoveryReason` 及访问接口尚不存在

**Step 3: Write minimal implementation**

新增：

- `enum class VesRecoveryReason`
- `ToString(VesRecoveryReason)`
- `VesClient::LastRecoveryReason()`
- 设置 reason 的内部 helper

这一步仍不接 heartbeat loop。

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_recovery_reason|ves_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/client/ves_dfx_types.h \
        virus_executor_service/src/client/ves_dfx_types.cpp \
        virus_executor_service/include/client/ves_client.h \
        virus_executor_service/src/client/ves_client.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp
git commit -m "feat: add ves recovery reason model"
```

### Task 4: Move Heartbeat Scheduling And Recovery Wiring Into `VesClient`

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Modify: `virus_executor_service/include/transport/ves_control_proxy.h`
- Modify: `virus_executor_service/src/transport/ves_control_proxy.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `VesClient::Init()` 后 heartbeat loop 启动
- heartbeat timeout 会调用 `RequestExternalRecovery()`
- heartbeat unhealthy 会调用 `RequestExternalRecovery()`
- heartbeat session mismatch 会调用 `RequestExternalRecovery()`
- heartbeat failure 不会直接走 `CloseSession`

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy'
```

Expected:

- FAIL，因为当前 heartbeat 没进 runtime 恢复闭环

**Step 3: Write minimal implementation**

实现策略：

- heartbeat loop 放在 `VesClient`
- `VesControlProxy` 继续只提供 `Heartbeat()` 调用和 socket death signal
- `VesClient` 获取 heartbeat reply 后完成：
  - reply 校验
  - reason 归因
  - 调 `client_.RequestExternalRecovery(...)`

禁止做法：

- 不在 heartbeat failure 里直接 `proxy_->CloseSession()`
- 不手动 fake `EngineDeathCallback`

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/client/ves_client.h \
        virus_executor_service/src/client/ves_client.cpp \
        virus_executor_service/include/transport/ves_control_proxy.h \
        virus_executor_service/src/transport/ves_control_proxy.cpp \
        virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp
git commit -m "feat: wire ves heartbeat through unified recovery path"
```

### Task 5: Enrich Heartbeat Health Snapshot

**Files:**
- Modify: `virus_executor_service/include/transport/ves_control_interface.h`
- Modify: `virus_executor_service/include/service/virus_executor_service.h`
- Modify: `virus_executor_service/src/service/virus_executor_service.cpp`
- Modify: `virus_executor_service/include/ves/ves_engine_service.h`
- Modify: `virus_executor_service/src/service/ves_engine_service.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_health_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- idle 时为 `OkIdle`
- 正常 in-flight 时为 `OkBusy`
- 长任务时为 `DegradedLongRunning`
- session 不存在时为 `UnhealthyNoSession`
- `reasonCode` 与 `status` 一致

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health|ves_heartbeat'
```

Expected:

- FAIL，因为当前 heartbeat 只有简单 `Ok/Unhealthy`

**Step 3: Write minimal implementation**

新增或扩展：

- `reasonCode`
- `flags`
- 服务端 health state helper

要求：

- 保持 reply 结构可解释
- 避免把所有 unhealthy 折叠成同一状态

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health|ves_heartbeat'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/transport/ves_control_interface.h \
        virus_executor_service/include/service/virus_executor_service.h \
        virus_executor_service/src/service/virus_executor_service.cpp \
        virus_executor_service/include/ves/ves_engine_service.h \
        virus_executor_service/src/service/ves_engine_service.cpp \
        virus_executor_service/tests/unit/ves/ves_health_test.cpp \
        virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp
git commit -m "feat: enrich ves heartbeat health snapshot"
```

### Task 6: Improve Soft ExecTimeout DFX Fidelity

**Files:**
- Modify: `memrpc/src/server/rpc_server.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 长执行期间 `lastHeartbeatMonoMs` 会推进
- timeout 归因为 `ExecTimeout`
- timeout 后不会被错误归因为 heartbeat failure

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|ves_policy'
```

Expected:

- FAIL，因为当前长执行 heartbeat 更新不够清晰

**Step 3: Write minimal implementation**

实现目标：

- 让 worker 执行长任务期间持续更新 runtime heartbeat
- client watchdog 保留更准确的 last state / timing 信息
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

### Task 7: Unify Supervisor And Harness Process Recovery Ownership

**Files:**
- Modify: `virus_executor_service/src/app/ves_supervisor_main.cpp`
- Modify: `virus_executor_service/src/app/ves_dt_crash_recovery_main.cpp`
- Modify: `virus_executor_service/src/app/ves_stress_client_main.cpp`
- Modify: `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`
- Create: `virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp`
- Create: `virus_executor_service/tests/dt/ves_idle_reopen_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing test**

补 DT 覆盖：

- heartbeat failure -> process respawn -> session reopen -> next call succeeds
- idle close -> next call reopen succeeds
- engine death 与 heartbeat overlap 时不会双重恢复
- supervisor 能 reap 并 respawn runtime-crashed engine

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|ves_heartbeat_recovery|ves_idle_reopen|virus_executor_service_supervisor_integration'
```

Expected:

- FAIL，因为 supervisor / harness 语义目前不统一

**Step 3: Write minimal implementation**

要求：

- `ves_supervisor_main` 增加 runtime `waitpid/reap/respawn`
- DT / stress harness 共用一致的 respawn 模式
- 明确日志：
  - detected
  - reaped
  - respawning
  - ready

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
        virus_executor_service/tests/dt/ves_crash_recovery_test.cpp \
        virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp \
        virus_executor_service/tests/dt/ves_idle_reopen_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "feat: unify ves process recovery ownership"
```

### Task 8: Align Documentation And Gates

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`
- Modify: `docs/demo_guide.md`
- Modify: `tools/push_gate.sh`
- Modify: `tools/ci_sweep.sh`
- Create: `docs/plans/2026-03-14-ves-dfx-recovery-closure.md`

**Step 1: Write the failing doc checklist**

列出必须改掉的旧说法：

- heartbeat failure 直接 close/restart 的混乱描述
- session replay 与 process respawn 边界不清
- 旧 idle callback / engine death 说明落后于当前代码

**Step 2: Verify the mismatch**

Run:

```bash
rg -n "heartbeat|replay|idle|engine death|InvokeSync" docs tools memrpc virus_executor_service
```

Expected:

- 能定位文档与设计不一致处

**Step 3: Write minimal updates**

文档必须明确：

- single recovery owner
- heartbeat -> `VesClient` -> `RpcClient::RequestExternalRecovery`
- `CloseSession` 只用于 intentional close
- process respawn 与 session replay 的边界

同时把相关 DFX DT case 纳入 gate。

**Step 4: Re-scan for consistency**

Run:

```bash
rg -n "RequestExternalRecovery|Heartbeat|CloseSession|Restart|replay" docs tools
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
git commit -m "docs: align ves dfx recovery architecture"
```

---

## 7. Verification Matrix

阶段性验证：

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_health|ves_policy|ves_recovery_reason|rpc_client_timeout_watchdog|rpc_client_idle_callback|rpc_client_external_recovery'
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

1. heartbeat 与 engine death 可能同时上报，导致双重恢复。
2. 新 heartbeat loop 可能带来 shutdown race。
3. 扩展 heartbeat reply 结构可能影响 mock SA 协议兼容。
4. supervisor runtime respawn 改动可能影响现有 demo 时序。
5. 如果把 process respawn 责任错误地下沉到 `RpcClient`，会再次把边界搅乱。

## 9. Mitigations

- 所有恢复请求统一走 `RpcClient::RequestExternalRecovery()` 或既有 `RecoveryPolicy` 动作。
- 所有 signal 都携带 reason，便于 de-dup 和日志解释。
- `CloseSession` 与 `Restart` 的语义通过测试先锁死，再改实现。
- heartbeat 只负责发现问题，不直接执行恢复。
- supervisor 只负责进程，不碰 session replay。

## 10. Recommended First Slice

如果先做最有价值的一小段，建议顺序：

1. Task 1
2. Task 2
3. Task 3
4. Task 4

这四步会先把最关键的语义收口：

- heartbeat 不再和 `CloseSession` / fake death callback 混用
- `RpcClient` 成为明确的恢复执行入口
- `VesClient` 成为 heartbeat 与 DFX reason 的汇聚点
- 整个系统开始有“清晰设计”，而不是“能跑但边界乱”
