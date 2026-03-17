# HarmonyOS SA 接入说明

## 分层原则

`memrpc` 的共享内存通信核心不依赖鸿蒙 SA 头文件。  
SA 相关逻辑只应出现在 bootstrap 层。

这样做的目的有两个：

- Linux 下可以独立开发、调试、跑 demo
- 迁移到 HarmonyOS 时只替换平台适配部分

## 当前仓库里的 SA 现状

当前仓库里的 `memrpc::SaBootstrapChannel` 不是正式鸿蒙实现，而是一个开发态 fake SA 适配层。

它的作用是：

- 把接口形状提前固定下来
- 在 Linux 下复用现有 POSIX 共享内存和 `eventfd` 通道
- 支持死亡回调、session 切换和恢复测试

## 鸿蒙正式环境下建议承担的职责

正式 SA 适配层建议负责：

- 发现或启动引擎服务
- 交换共享内存 fd
- 交换请求/响应 `eventfd`
- 暴露通用 `CheckHealth(expectedSessionId)`，把业务 heartbeat/probe 翻译成 generic health result
- 把子进程死亡事件回调给 `RpcClient`
- 在重启后让 `OpenSession()` 返回最新 `BootstrapHandles`

## 建议接入流程

1. 客户端通过 `GetSystemAbility` 获取服务代理
2. 需要拉起或重建引擎时调用 `LoadSystemAbility`
3. 通过 SA 交换 `BootstrapHandles`
4. `RpcClient` 使用这些句柄附着共享内存会话
5. `RpcClient` watchdog 周期性执行 `CheckHealth()`
6. 若健康检查超时/异常，`RpcClient::RequestExternalRecovery()` 统一导入 restart
7. 若引擎死亡，SA 回调通知客户端
8. 下一次业务调用在 cooldown 结束后自动重新附着新 session；若是 idle close，则按 demand reconnect reopen

## 当前恢复语义

当前代码实现的是单恢复 owner 模型：

- 死亡回调到来后，旧 `session` 立即失效
- heartbeat / engine death / timeout / idle close / manual shutdown 都先进入 `RpcClient` 的统一 lifecycle state machine
- `Shutdown()` 是唯一 public terminal close，进入 `Closed` 后不会再接受 recovery
- `CloseSession` 只用于 non-terminal intentional close，例如 idle unload；该路径进入 `IdleClosed`
- `Restart` 只用于 fault recovery；带 delay 时进入 `Cooldown`，实际 reopen 时进入 `Recovering`
- replay-safe 请求和 DFX/testkit replay 决策都消费统一 recovery snapshot/report

## 生命周期与 trigger 语义

框架内部统一使用以下 trigger：

- `ManualShutdown`
- `ExecTimeout`
- `EngineDeath`
- `ExternalHealthSignal`
- `IdlePolicy`
- `DemandReconnect`

对 SA 适配层的要求是：

- 只负责把 SA/heartbeat/子进程死亡翻译成 `EngineDeath` 或 `ExternalHealthSignal`
- 不在 SA 适配层自行维护额外 cooldown 或 retry 状态机
- 不把 manual shutdown 和 idle close 混成同一种 “session closed” 事件

业务侧若需要观测恢复过程，应读取 `RecoveryRuntimeSnapshot` / `RecoveryEventReport`，而不是依赖分散的布尔状态。

对于 VES：

- heartbeat 协议和 `VesHeartbeatReply` 仍留在 `VesControlProxy`
- `memrpc` core 不依赖业务 heartbeat 结构
- `VesClient` 可选订阅完整 VES snapshot，但该 side-channel 不参与恢复判定

这个边界比较适合先接入业务，再由上层决定是否做更激进的重试策略。
