# HarmonyOS SA 接入说明

## 分层原则

`memrpc` 的共享内存通信核心不依赖鸿蒙 SA 头文件。  
SA 相关逻辑只应出现在 bootstrap 层。

这样做的目的有两个：

- Linux 下可以独立开发、调试、跑示例
- 迁移到 HarmonyOS 时只替换平台适配部分

## 当前仓库里的 SA 现状

当前仓库里的 `memrpc::SaBootstrapChannel` 不是正式鸿蒙实现，而是一个开发态的模拟 SA 适配层。

它的作用是：

- 把接口形状提前固定下来
- 在 Linux 下复用现有 POSIX 共享内存和 `eventfd` 通道
- 支持死亡回调、session 切换和恢复测试

## 鸿蒙正式环境下建议承担的职责

正式 SA 适配层建议负责：

- 发现或启动引擎服务
- 交换共享内存 fd
- 交换请求/响应 `eventfd`
- 暴露通用 `CheckHealth(expectedSessionId)`，把业务 heartbeat/probe 翻译成通用健康结果
- 把子进程死亡事件回调给 `RpcClient`
- 在重启后让 `OpenSession()` 返回最新 `BootstrapHandles`

这里有一条边界要保持明确：

- 死亡回调只负责报告“旧对象已经失效”
- 不要求在死亡回调里同步拿到下一份 control
- 下一次 `OpenSession()` 或显式 control 取用时，再按需刷新到新 control

## 建议接入流程

1. 客户端通过 `GetSystemAbility` 获取服务代理
2. 需要拉起或重建引擎时调用 `LoadSystemAbility`
3. 通过 SA 交换 `BootstrapHandles`
4. `RpcClient` 使用这些句柄附着共享内存会话
5. `RpcClient` watchdog 周期性执行 `CheckHealth()`
6. 若健康检查超时或异常，`RpcClient::RequestExternalRecovery()` 统一导入恢复
7. 若引擎死亡，SA 回调通知客户端
8. 下一次业务调用在 cooldown 结束后自动重新附着新 session；若是 idle close 或恢复失败后的 `Disconnected`，则按需重连重新进入新 session

## 当前恢复语义

维护者若需要先确认“谁拥有恢复生命周期、哪些只是兼容视图、VES 能有几个旁路”，可先读
[recovery_ownership.md](/root/mem/docs/recovery_ownership.md)。

当前代码实现的是单恢复所有者模型：

- 死亡回调到来后，旧 `session` 立即失效
- bootstrap control 在死亡后只进入失效态，不在通知回调里偷偷做恢复预热
- heartbeat / engine death / timeout / idle close / manual shutdown 都先进入 `RpcClient` 的统一生命周期状态机
- `Shutdown()` 是唯一的公开终态关闭入口，进入 `Closed` 后不会再接受恢复
- `CloseSession` 只用于非终态的主动关闭，例如 idle unload；该路径进入 `IdleClosed`
- 恢复尝试若无法成功 reopen，会进入 `Disconnected`，等待下一次真实调用再触发 reopen
- `Restart` 只用于故障恢复；带 delay 时进入 `Cooldown`，实际 reopen 时进入 `Recovering`
- replay-safe 请求和 DFX/testkit replay 决策都消费统一 recovery snapshot/report

## 生命周期与触发源语义

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
- `memrpc` 核心层不依赖业务 heartbeat 结构
- `VesClient` 可选订阅完整 VES snapshot，但这个观测通道不参与恢复判定
- `VesClient` 最多只有一个业务旁路，即 `AnyCall` 这类传输降级；恢复等待和重试应复用 `RpcClient` 的通用能力，而不是在业务封装层再维护一套生命周期/重试逻辑

这个边界比较适合先接入业务，再由上层决定是否做更激进的重试策略。
