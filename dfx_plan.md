# DFX And Recovery Closure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 收口 virus executor service 的 DFX / 心跳 / 恢复链路，使 idle、heartbeat、engine death、exec-timeout、supervisor respawn 形成一致且可诊断的闭环。

**Architecture:** 以 `MemRpc::RpcClient` 的 `RecoveryPolicy` 为恢复决策核心，以 `VesControlProxy` 的控制 socket 为 liveness/heartbeat 通道，以 `VirusExecutorService::Heartbeat()` 的 health snapshot 为服务端健康事实来源。；本轮重点是把现有零散机制收口成一致的运行时策略、状态模型、日志和测试矩阵。

**Tech Stack:** C++17, shared memory, eventfd, Unix-domain socket, `MemRpc::RecoveryPolicy`, OHOS SA mock transport, GoogleTest, DT/stress/integration tests

---

## Background

当前仓库已经具备几块 DFX / 恢复能力，但边界还比较散：

- `RpcClient` 已经支持 `onFailure / onIdle / onEngineDeath` 三类恢复决策，并具备强制重启、重放 safe calls、poison-pill fail 的能力。
- `VirusExecutorService` 已经提供 heartbeat reply，包括 `sessionId / inFlight / lastTaskAgeMs / currentTask`。
- `VesControlProxy` 已经能通过 cmd=3 发 heartbeat，也能通过 monitor socket 感知 peer disconnect。
- `VesClient` 目前只配置了 `RecoveryPolicy`，还没有真正把 heartbeat 纳入运行时恢复闭环。
- `ExecTimeout` 目前仍是软超时，客户端会超时返回，但服务端 worker 不会因此被强制中断。
- supervisor / DT harness / stress harness 对 crash respawn 的 ownership 不一致，导致 DFX 路径看起来“有功能”，但运行时谁负责恢复、谁负责记录、谁负责判定 unhealthy，仍然比较混乱。

## Problem Statement

本轮不是补一个孤立 feature，而是要解决下面这些实际问题：

1. 心跳存在，但没有进入 `VesClient` 的真实恢复路径。
2. idle / heartbeat unhealthy / engine death / exec-timeout 四类信号没有统一归一成一致的 DFX 事件和恢复动作。
3. 服务端 health snapshot 还偏轻，难以清晰表达“空闲”“正常忙”“长时间执行”“已失去会话”“等待 unload”等状态。
4. supervisor 和 harness 的恢复责任边界不清晰，导致现场定位时很难判断问题是在 framework、transport、还是 process supervision。
5. 文档和代码已经部分偏离，容易导致后续实现继续分叉。

## Scope

本计划覆盖：

- client heartbeat 调度与 unhealthy 判定
- health snapshot / reason code / DFX event 模型
- `RecoveryPolicy` 与 heartbeat/death/failure 的收口
- VES supervisor / harness 的恢复 ownership 整理
- 日志、状态、测试、文档的一致性打磨

本计划不覆盖：

- 重新设计 shared-memory wire protocol
- 真正的“硬 kill handler”能力
- 多 client / 多 session 支持
- 高阶 dashboard / metrics 平台接入
- `problem1` 根因修复本身

## Target State

完成后应满足：

1. `VesClient` 能定期 heartbeat，并把 timeout / malformed / unhealthy / stale session 判定纳入恢复闭环。
2. 所有恢复动作都能归因到统一的 DFX reason，例如 `IdleClose`、`HeartbeatTimeout`、`HeartbeatUnhealthy`、`EngineDeath`、`ExecTimeout`。
3. 服务端 heartbeat 能稳定暴露“当前健康状态 + 正在执行什么 + 最近卡住多久 + 是否有 live session”。
4. supervisor / DT / stress harness 对 crash respawn 的行为和日志口径一致。
5. 文档能准确反映当前实现，不再出现“代码已经自动 replay，但文档还说不会 replay”这类漂移。

## Guiding Principles

- 先统一状态语义，再加恢复动作。
- heartbeat 只做 cheap liveness / health check，不承载业务流量。
- 恢复 owner 明确，避免 framework 和 supervisor 同时做半套恢复。
- 保留现有 `RpcClient` 恢复骨架，优先收口，不做大重写。
- 所有新行为必须有 focused test，不靠手工跑日志验证。

## Proposed DFX Model

### 1. Recovery Trigger Taxonomy

统一定义恢复触发原因：

- `IdleThresholdReached`
- `HeartbeatTimeout`
- `HeartbeatMalformed`
- `HeartbeatUnhealthy`
- `HeartbeatSessionMismatch`
- `EngineDeathSocketDisconnect`
- `ExecTimeout`
- `QueueTimeout`
- `ProtocolMismatch`
- `ManualClose`

### 2. Health Status Taxonomy

服务端 heartbeat 返回的健康状态建议明确为：

- `OkIdle`
- `OkBusy`
- `DegradedLongRunning`
- `UnhealthyNoSession`
- `UnhealthyServerStopping`
- `UnhealthyInternalError`

如果 wire 兼容性要求当前 `status` 仍只能保留 `Ok/Unhealthy`，则至少增加 `reasonCode` 字段，避免把所有 unhealthy 折叠成一个 bit。

### 3. Ownership Rules

- `RpcClient` 负责 session 级恢复、replay-safe 请求恢复、cooldown gate。
- `VesControlProxy` 负责 transport liveness 输入，不直接决定复杂恢复策略。
- `VesClient` 负责把 heartbeat / death / idle / timeout 统一映射到 `RecoveryPolicy` 和 DFX 日志。
- supervisor / harness 负责 process respawn，不负责 session replay。

## Execution Order

建议顺序：

1. 先补 specification-style tests 和 DFX 状态模型。
2. 再接 client heartbeat runtime。
3. 再扩 health snapshot / reason code。
4. 再统一 supervisor / DT / stress harness 行为。
5. 最后更新文档与 push gate。

---

### Task 1: Freeze DFX Semantics And Add Focused Test Matrix

**Files:**
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Modify: `memrpc/tests/rpc_client_idle_callback_test.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Create: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`

**Step 1: Write the failing tests**

补下面几类测试，先把语义锁死：

- heartbeat timeout 会触发恢复，不只是返回错误
- heartbeat unhealthy 会触发恢复
- stale `sessionId` heartbeat 会触发恢复
- idle close 与 heartbeat failure 在日志/事件 reason 上可区分
- `ExecTimeout` 与 `HeartbeatTimeout` 的恢复 reason 不会混淆

测试建议最小覆盖：

```cpp
TEST(VesRecoveryReasonTest, HeartbeatTimeoutMapsToRecoveryReason);
TEST(VesRecoveryReasonTest, HeartbeatUnhealthyMapsToRecoveryReason);
TEST(VesRecoveryReasonTest, SessionMismatchMapsToRecoveryReason);
TEST(VesRecoveryReasonTest, IdleCloseMapsToRecoveryReason);
```

**Step 2: Run tests to verify they fail**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_idle_callback|rpc_client_timeout_watchdog'
```

Expected:

- FAIL，因为当前代码还没有统一的 recovery reason 模型，也没有把 heartbeat 接入恢复闭环

**Step 3: Write minimal scaffolding**

最小实现只先补：

- recovery reason enum / helper 接口
- test seam
- 空实现或默认返回，先让编译通过

不要在这一步就接 heartbeat runtime。

**Step 4: Run tests to verify the right failures remain**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_recovery_reason|rpc_client_idle_callback|rpc_client_timeout_watchdog'
```

Expected:

- 编译通过
- 行为断言继续失败，说明测试真正卡在语义缺失而不是编译错误

**Step 5: Commit**

```bash
git add virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp \
        memrpc/tests/rpc_client_idle_callback_test.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        virus_executor_service/CMakeLists.txt
git commit -m "test: freeze ves dfx recovery semantics"
```

### Task 2: Introduce A Unified VES DFX Reason Model

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Create: `virus_executor_service/include/client/ves_dfx_types.h`
- Create: `virus_executor_service/src/client/ves_dfx_types.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `VesClient` 能记录最近一次恢复原因
- `ExecTimeout`、`EngineDeath`、`IdleClose`、`HeartbeatTimeout` 对应不同 reason
- 默认 reason 为 `Unknown`

示意：

```cpp
EXPECT_EQ(client.LastRecoveryReason(), VesRecoveryReason::Unknown);
```

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_policy|ves_recovery_reason'
```

Expected:

- FAIL，因为 `VesRecoveryReason` 与 `LastRecoveryReason()` 尚不存在

**Step 3: Write minimal implementation**

新增：

- `enum class VesRecoveryReason`
- `const char* ToString(VesRecoveryReason)`
- `VesClient` 内部记录最近一次 reason
- 把现有 `onFailure` / `onEngineDeath` / `onIdle` 先映射到 reason

这一步还不接 heartbeat，只先收口已存在三类信号。

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_policy|ves_recovery_reason'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/include/client/ves_client.h \
        virus_executor_service/src/client/ves_client.cpp \
        virus_executor_service/include/client/ves_dfx_types.h \
        virus_executor_service/src/client/ves_dfx_types.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp \
        virus_executor_service/tests/unit/ves/ves_recovery_reason_test.cpp
git commit -m "feat: add unified ves recovery reason model"
```

### Task 3: Wire Runtime Heartbeat Into `VesClient`

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Modify: `virus_executor_service/include/transport/ves_control_proxy.h`
- Modify: `virus_executor_service/src/transport/ves_control_proxy.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `VesClient::Init()` 后启动 heartbeat loop
- heartbeat timeout 时会触发 `CloseSession + recovery`
- heartbeat unhealthy 时会触发 `CloseSession + recovery`
- heartbeat session mismatch 时会触发 `CloseSession + recovery`

建议优先通过 mock seam 驱动 `VesControlProxy::Heartbeat()` 返回：

- `PeerDisconnected`
- `ProtocolMismatch`
- `status = Unhealthy`
- `sessionId != expected`

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy'
```

Expected:

- FAIL，因为当前 `VesClient` 没有 heartbeat 调度线程/循环

**Step 3: Write minimal implementation**

实现策略：

- 在 `VesClient` 内新增 heartbeat worker，默认按固定周期工作
- 如果 `VesControlProxy` 已有 monitor thread 可复用，优先复用其调度点，而不是再发散出第二套长期线程
- heartbeat failure 不直接在 proxy 内硬编码恢复策略，而是回调到 `VesClient`，再由 `VesClient` 统一设置 recovery reason、记录日志并触发 close/restart
- 校验项至少包括：
  - `Heartbeat()` 返回状态
  - reply size / version
  - `reply.status`
  - `reply.sessionId == current session`

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
git commit -m "feat: wire ves heartbeat into runtime recovery"
```

### Task 4: Enrich Heartbeat Reply Into A Real Health Snapshot

**Files:**
- Modify: `virus_executor_service/include/transport/ves_control_interface.h`
- Modify: `virus_executor_service/include/service/virus_executor_service.h`
- Modify: `virus_executor_service/src/service/virus_executor_service.cpp`
- Modify: `virus_executor_service/include/ves/ves_engine_service.h`
- Modify: `virus_executor_service/src/service/ves_engine_service.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_health_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`

**Step 1: Write the failing test**

把 heartbeat / health snapshot 语义补足：

- idle 时返回 `OkIdle`
- 有 in-flight 时返回 `OkBusy`
- 长时间执行超过阈值时返回 `DegradedLongRunning`
- session/service 不存在时返回 `UnhealthyNoSession`

如果 wire 不便扩 `status`，则至少测 `reasonCode`。

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_health|ves_heartbeat'
```

Expected:

- FAIL，因为当前 heartbeat 只有 `Ok/Unhealthy`

**Step 3: Write minimal implementation**

新增最小字段：

- `reasonCode`
- 可选 `stateVersion`
- 可选 `lastHeartbeatHandledMs`

服务端 health snapshot 计算逻辑补齐：

- 是否 initialized
- 是否存在 live session
- `inFlight`
- `lastTaskAgeMs`
- 长时间执行阈值

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

### Task 5: Close The Gap Between Soft ExecTimeout And Hang DFX

**Files:**
- Modify: `memrpc/include/memrpc/core/protocol.h`
- Modify: `memrpc/src/server/rpc_server.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- 当 handler 长时间执行时，slot runtime 的 `lastHeartbeatMonoMs` 会持续推进
- client watchdog 能区分“已经超时但 server 仍存活在执行”与“peer disconnect”
- DFX reason 为 `ExecTimeout`，而不是错误归因成 heartbeat failure

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|ves_policy'
```

Expected:

- FAIL，因为当前 `lastHeartbeatMonoMs` 基本只在阶段切换时更新

**Step 3: Write minimal implementation**

最小策略：

- worker 执行长任务期间周期性更新 slot runtime heartbeat
- client watchdog 判断 timeout 时同时保留最近 runtime state
- 明确日志：这是“soft exec timeout with live worker”，不是 engine death

注意：

- 这一步不做强杀 worker
- 不修改既有 `ExecTimeout` 语义，只提高 DFX 可解释性

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'rpc_client_timeout_watchdog|ves_policy'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add memrpc/include/memrpc/core/protocol.h \
        memrpc/src/server/rpc_server.cpp \
        memrpc/src/client/rpc_client.cpp \
        memrpc/tests/rpc_client_timeout_watchdog_test.cpp \
        virus_executor_service/tests/unit/ves/ves_policy_test.cpp
git commit -m "feat: improve exec-timeout hang dfx fidelity"
```

### Task 6: Unify Supervisor And Harness Recovery Ownership

**Files:**
- Modify: `virus_executor_service/src/app/ves_supervisor_main.cpp`
- Modify: `virus_executor_service/src/app/ves_dt_crash_recovery_main.cpp`
- Modify: `virus_executor_service/src/app/ves_stress_client_main.cpp`
- Modify: `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`
- Modify: `virus_executor_service/tests/stress/testkit_stress_runner.cpp`

**Step 1: Write the failing test**

新增 focused test / harness assertion：

- engine crash 后 supervisor 能 reap 并 respawn
- respawn 行为与 DT/stress harness 保持一致
- 日志口径统一输出：
  - crash detected
  - process reaped
  - respawn started
  - respawn ready

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|virus_executor_service_supervisor_integration|virus_executor_service_testkit_dt_stability'
```

Expected:

- FAIL 或行为缺失，因为 `ves_supervisor_main` 当前没有 runtime respawn 闭环

**Step 3: Write minimal implementation**

最小实现：

- 给 `ves_supervisor_main` 加 `waitpid(..., WNOHANG)` / reap 逻辑
- 把 DT harness 中已存在的 respawn 模式抽成一致 helper
- 明确 process-level ownership：supervisor/harness 负责 respawn，`RpcClient` 只负责 session 恢复

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|virus_executor_service_supervisor_integration|virus_executor_service_testkit_dt_stability'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add virus_executor_service/src/app/ves_supervisor_main.cpp \
        virus_executor_service/src/app/ves_dt_crash_recovery_main.cpp \
        virus_executor_service/src/app/ves_stress_client_main.cpp \
        virus_executor_service/tests/dt/ves_crash_recovery_test.cpp \
        virus_executor_service/tests/stress/testkit_stress_runner.cpp
git commit -m "feat: unify ves supervisor and harness recovery ownership"
```

### Task 7: Add End-To-End DFX Regression Coverage

**Files:**
- Modify: `tools/push_gate.sh`
- Modify: `tools/ci_sweep.sh`
- Modify: `virus_executor_service/CMakeLists.txt`
- Create: `virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp`
- Create: `virus_executor_service/tests/dt/ves_idle_close_reopen_test.cpp`

**Step 1: Write the failing tests**

新增 DT 用例：

- heartbeat unhealthy -> close -> reopen -> first normal call succeeds
- idle close -> next call auto reopen succeeds
- engine death + heartbeat overlap 时不会双重恢复

**Step 2: Run test to verify it fails**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat_recovery|ves_idle_close_reopen'
```

Expected:

- FAIL，因为当前没有这些 DT 回归用例

**Step 3: Write minimal implementation**

补 CMake 和 gate：

- 把新 DT case 纳入 `ctest -L dt`
- `push_gate.sh` 至少覆盖 heartbeat / crash recovery / idle close 的子集
- `ci_sweep.sh` 把 DFX 回归放进深度验证路径

**Step 4: Run test to verify it passes**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat_recovery|ves_idle_close_reopen|virus_executor_service_crash_recovery'
```

Expected:

- PASS

**Step 5: Commit**

```bash
git add tools/push_gate.sh \
        tools/ci_sweep.sh \
        virus_executor_service/CMakeLists.txt \
        virus_executor_service/tests/dt/ves_heartbeat_recovery_test.cpp \
        virus_executor_service/tests/dt/ves_idle_close_reopen_test.cpp
git commit -m "test: add ves dfx end-to-end regression coverage"
```

### Task 8: Update Architecture And DFX Docs

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/sa_integration.md`
- Modify: `docs/demo_guide.md`
- Create: `docs/plans/2026-03-14-ves-dfx-recovery-closure.md`

**Step 1: Write the failing doc checklist**

列出当前不一致点：

- 文档还在描述旧恢复模型
- heartbeat 已实现但未描述 runtime 接入方式
- replay-safe 恢复与 process respawn ownership 未写清楚

**Step 2: Verify the mismatch**

Run:

```bash
rg -n "InvokeSync|idle callback|will not be auto-replayed|heartbeat" docs memrpc virus_executor_service
```

Expected:

- 能看到现有文档与代码不一致的描述

**Step 3: Write minimal doc updates**

更新重点：

- 当前恢复 owner
- heartbeat runtime 流程图
- DFX reason 表
- soft exec-timeout 与 hard hang 的区别
- supervisor / session recovery 边界

**Step 4: Review docs for consistency**

Run:

```bash
rg -n "RecoveryPolicy|Heartbeat|replay|Idle" docs/architecture.md docs/sa_integration.md docs/demo_guide.md docs/plans/2026-03-14-ves-dfx-recovery-closure.md
```

Expected:

- 关键术语一致

**Step 5: Commit**

```bash
git add docs/architecture.md \
        docs/sa_integration.md \
        docs/demo_guide.md \
        docs/plans/2026-03-14-ves-dfx-recovery-closure.md
git commit -m "docs: align ves dfx recovery architecture"
```

---

## Verification Matrix

每完成 2-3 个 task 至少跑一次：

```bash
tools/build_and_test.sh --test-regex 'ves_heartbeat|ves_policy|ves_health|rpc_client_timeout_watchdog|rpc_client_idle_callback'
```

进入 DT 阶段后运行：

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_crash_recovery|ves_heartbeat_recovery|ves_idle_close_reopen'
```

在 DFX / recovery 主线完成后运行：

```bash
tools/push_gate.sh --deep
```

## Risks

1. heartbeat 和 monitor socket 可能重复触发恢复，造成双重 restart。
2. 引入新的 heartbeat worker 后，shutdown 顺序容易再次出现 race。
3. 扩展 heartbeat reply 结构如果处理不好，会带来 mock SA 协议兼容问题。
4. supervisor 改动可能影响现有 demo / integration test 的启动时序。
5. 如果试图在本轮强行做“硬 kill hang worker”，会显著扩大范围。

## Mitigations

- 所有恢复入口统一走 `VesClient` 的 reason 标记和 gating。
- heartbeat runtime 先做单入口，再考虑复用线程实现细节。
- 所有新 reason 都要求 unit test + DT test 各至少一个。
- 先保留 wire backward-compatible 扩展；必须改 struct 时同步更新 stub/proxy/tests。
- 明确本轮只提高 hang DFX fidelity，不实现 hard cancel。

## Suggested First Slice

如果只先做一小段，建议顺序：

1. Task 1
2. Task 2
3. Task 3
4. Task 4

这四步完成后，就能把“heartbeat 已存在但未接入恢复闭环”的核心缺口补上，而且不需要先碰 supervisor 重构。
