# RpcClient 客户端生命周期状态机

## 1. 状态定义

| 状态 | 值 | 含义 |
|------|-----|------|
| **Uninitialized** | 0 | 初始状态，未初始化 |
| **Active** | 1 | 正常服务状态，有活跃会话 |
| **NoSession** | 2 | 无会话状态，恢复失败或策略Ignore后进入 |
| **Cooldown** | 3 | 冷却中，等待delayMs后恢复 |
| **IdleClosed** | 4 | 空闲关闭，onIdle策略IdleClose后进入 |
| **Recovering** | 5 | 恢复中，正在尝试建立新会话 |
| **Closed** | 6 | 终端关闭，Shutdown后进入 |

## 2. 触发事件分类

### 2.1 用户操作
| 事件 | 描述 |
|------|------|
| **Init()** | 客户端初始化 |
| **NewRequest** | 新请求进来，调用EnsureLiveSession() |
| **Shutdown()** | 用户主动关闭客户端 |

### 2.2 异常事件
| 事件 | 描述 |
|------|------|
| **EngineDeath** | 引擎进程死亡（abort/exit） |
| **ExecTimeout** | 请求执行超时 |
| **SessionOpenFailure** | 会话建立失败 |
| **TransportFailure** | 传输层失败（PeerDisconnected） |

### 2.3 策略回调决策
| 策略 | 可能的决策 |
|------|-----------|
| **onEngineDeath** | Restart(delayMs), Ignore |
| **onFailure** | Restart(delayMs), Ignore |
| **onIdle** | IdleClose, Ignore |

### 2.4 内部定时事件
| 事件 | 描述 |
|------|------|
| **CooldownExpired** | 冷却时间到 |
| **RecoveryTick** | Recovering状态下的重试节拍 |

---

## 3. 完整状态转换表

### 3.1 从 Uninitialized 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Init() | - | Recovering | BeginSessionOpen(), 尝试OpenSession() |
| Shutdown() | - | Closed | 进入Closed |

### 3.2 从 Active 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Shutdown() | - | Closed | CloseLiveSession(), EnterTerminalClosed() |
| EngineDeath | onEngineDeath=Restart(delay>0) | Cooldown | CloseLiveSession(), ResolveAllPending(CrashedDuringExecution), StartRecovery(delayMs) |
| EngineDeath | onEngineDeath=Restart(delay=0) | Recovering | CloseLiveSession(), ResolveAllPending(CrashedDuringExecution), StartRecovery(0) |
| EngineDeath | onEngineDeath=Ignore / no policy | NoSession | CloseLiveSession(), ResolveAllPending(CrashedDuringExecution), EnterNoSession() |
| ExecTimeout | onFailure=Restart(delay>0) | Cooldown | StartRecovery(delayMs), CloseLiveSessionIfSnapshotMatches() |
| ExecTimeout | onFailure=Restart(delay=0) | Recovering | StartRecovery(0), CloseLiveSessionIfSnapshotMatches() |
| ExecTimeout | onFailure=Ignore / no policy | (保持Active) | 不触发恢复，仅标记失败 |
| onIdle=IdleClose | idle超时 | IdleClosed | CloseLiveSession(), EnterIdleClosed() |
| HealthCheck失败 | - | Recovering | HandleExternalRecovery(delay=0) |

### 3.3 从 Cooldown 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Shutdown() | - | Closed | 进入Closed |
| CooldownExpired | delayMs=0 | Recovering | 自动进入Recovering，尝试OpenSession() |
| CooldownExpired | delayMs>0, 新请求触发 | Recovering | EnsureLiveSession()被调用，检测到Cooldown结束，进入Recovering |
| NewRequest | - | (保持Cooldown) | WaitForCooldownToSettle()等待，不触发新拉起 |
| EngineDeath | 当前sessionId匹配 | (更新Cooldown) | 重新StartRecovery()，可能更新cooldownUntilMs |

### 3.4 从 Recovering 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Shutdown() | - | Closed | 进入Closed |
| OpenSession()成功 | - | Active | FinalizeSessionOpen() |
| OpenSession()失败 | - | NoSession | HandleSessionOpenFailure() |
| NewRequest | - | (保持Recovering) | WaitOneRecoveryRetryTick()等待 |
| EngineDeath | sessionId匹配 | (保持Recovering) | 重新HandleEngineDeathLocked() |

### 3.5 从 NoSession 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Shutdown() | - | Closed | 进入Closed |
| NewRequest | - | Recovering | EnsureLiveSession()触发 BeginSessionOpen() 并尝试 OpenSession()；若再次失败则回到 NoSession |
| 外部调用RequestExternalRecovery | - | Recovering/Cooldown | HandleExternalRecovery() |

### 3.6 从 IdleClosed 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| Shutdown() | - | Closed | 进入Closed |
| NewRequest | - | Recovering | EnsureLiveSession()检测到IdleClosed，BeginSessionOpen() |
| onIdle=Restart | - | Recovering | 自动恢复 |

### 3.7 从 Closed 状态

| 触发事件 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| 任何事件 | - | Closed | 终态，不再变化 |

---

## 4. 关键行为详解

### 4.1 StartRecovery(delayMs) 的行为

```
StartRecovery(delayMs):
    cooldownUntilMs = now + delayMs
    if delayMs == 0:
        状态 = Recovering
    else:
        状态 = Cooldown
    CloseLiveSessionIfSnapshotMatches()
    HandlePendingRequestsForRecovery(pendingAction)
    MaybeReconnectImmediatelyLocked(delayMs)  // 如果delay=0立即尝试重连
```

### 4.2 EnsureLiveSession() 的行为

```
EnsureLiveSession():
    if 状态 == Closed: return ClientClosed
    if CooldownActive(): return CooldownActive
    if HasLiveSession(): return Ok
    
    // 需要建立新会话
    BeginSessionOpen(当前状态, currentSessionId)
    status = OpenSessionLocked()
    if status != Ok:
        HandleSessionOpenFailure(status)
    return status
```

### 4.3 HandleEngineDeath() 的行为

```
HandleEngineDeath(observedSessionId, deadSessionId):
    if observedSessionId == 0: return  // 忽略，会话已关闭
    if sessionId不匹配: return  // 忽略过期的死亡通知
    
    CloseLiveSessionIfSnapshotMatches(observedSessionId)
    ResolveAllPending(CrashedDuringExecution)
    
    policy = LoadRecoveryPolicy()
    if !policy.onEngineDeath:
        EnterNoSession()
        return
    
    decision = policy.onEngineDeath(report)
    if decision.action == Ignore:
        EnterNoSession()
    else if decision.action == Restart:
        ApplyRecoveryDecisionLocked(Restart, delayMs)
```

---

## 5. 策略决策与状态转换关系

### 5.1 onEngineDeath 策略

| 引擎死亡触发 | 策略决策 | 目标状态 | 新请求行为 |
|--------------|----------|----------|------------|
| EngineDeath | Restart(200ms) | Cooldown | 等待冷却结束后自动恢复 |
| EngineDeath | Restart(0) | Recovering | 立即尝试恢复 |
| EngineDeath | Ignore | NoSession | 新请求快速失败，需外部触发恢复 |
| EngineDeath | 未配置 | NoSession | 同上 |

### 5.2 onFailure 策略（针对ExecTimeout）

| 超时触发 | 策略决策 | 目标状态 | 说明 |
|----------|----------|----------|------|
| ExecTimeout | Restart(200ms) | Cooldown | 进入冷却，然后恢复 |
| ExecTimeout | Restart(0) | Recovering | 立即恢复 |
| ExecTimeout | Ignore | (保持Active) | 仅标记请求失败，不恢复 |
| ExecTimeout | 未配置 | (保持Active) | 同上 |

### 5.3 onIdle 策略

| 空闲触发 | 策略决策 | 目标状态 | 新请求行为 |
|----------|----------|----------|------------|
| idle超时 | IdleClose | IdleClosed | 新请求触发恢复到Active |
| idle超时 | Ignore | (保持Active) | 保持会话 |
| idle超时 | 未配置 | (保持Active) | 同上 |

---

## 6. 状态转换图（文字描述）

```
                         +-----------+
                         | Uninitialized |
                         +-----+-----+
                               |
                         Init()|
                               v
                         +-----+-----+
                    +--->| Recovering|<------------------+
                    |    +-----+-----+                   |
                    |          |                         |
              成功   |    失败  |                         |
                    |          v                         |
                    |    +-----------+                   |
                    +----|   Active    |<-------------+   |
                    |    +-----+-----+               |   |
                    |          |                     |   |
         Shutdown() |          | EngineDeath/        |   |
                    |          | ExecTimeout         |   |
                    |          | (Restart策略)       |   |
                    |          v                     |   |
                    |    +-----------+               |   |
                    +--->|  Cooldown  |               |   |
                    |    +-----+-----+               |   |
                    |          | CooldownExpired     |   |
                    |          | or 新请求触发        |   |
                    |          v                     |   |
                    |    +-----------+               |   |
                    +--->| Recovering |---------------+   |
                    |    +-----+-----+                   |
              成功   |          |                         |
                    |    失败   |                         |
                    |          v                         |
                    |    +-----------+ 外部触发恢复      |
                    +--->|  NoSession  |+-----------------+
                    |    +-----+-----+
                    |          |
         Shutdown() |          | onIdle=IdleClose
                    |          v
                    |    +-----------+
                    +--->| IdleClosed  |<--+
                    |    +-----+-----+     |
                    |          |           |
                    |          | 新请求     |
                    |          v           |
                    +------->| Recovering|--+
                    |        +-----------+
                    |
                    v
               +---------+
               |  Closed | (终态)
               +---------+
```

---

## 7. 关键概念澄清

### 7.1 Cooldown vs Recovering 的区别

| 特性 | Cooldown | Recovering |
|------|----------|------------|
| 目的 | 延迟恢复，避免抖动 | 立即尝试恢复 |
| 触发条件 | Restart策略delayMs>0 | Restart策略delayMs=0 或 Cooldown结束 |
| 新请求行为 | 阻塞等待冷却结束 | 阻塞等待恢复完成 |
| 是否尝试OpenSession | 否 | 是 |

### 7.2 NoSession vs IdleClosed 的区别

| 特性 | NoSession | IdleClosed |
|------|-----------|------------|
| 触发原因 | 恢复失败或策略Ignore | 空闲超时onIdle=IdleClose |
| 新请求行为 | 触发一次重新 OpenSession；失败则返回失败 | 触发恢复到Active |
| 是否可自动恢复 | 可以由新请求驱动重试，但失败后仍停留/回到 NoSession | 是（新请求触发） |

### 7.3 RecoveryPending 的含义

```cpp
bool RecoveryPending() const {
    return lifecycleState == Recovering || lifecycleState == Cooldown;
}
```

表示正在恢复过程中（Cooldown或Recovering）。

---

## 8. 并发安全说明

所有状态转换都在 `lifecycleMutex_` 锁保护下进行。关键方法：

- `TransitionLifecycle()` - 原子状态转换
- `ScheduleRecoveryLocked()` - 调度恢复
- `EnsureLiveSessionLocked()` - 确保会话
- `HandleEngineDeathLocked()` - 处理引擎死亡
- `ApplyRecoveryDecisionLocked()` - 应用恢复决策

状态读取使用 `LifecycleState()` 获取快照，可能立即过时，仅用于非关键决策。
