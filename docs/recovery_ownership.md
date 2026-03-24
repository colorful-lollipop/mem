# 恢复职责说明

## 目标

这份说明用来把当前恢复模型的职责边界讲清楚，避免维护者继续把生命周期、业务旁路和兼容接口混在一起理解。

一句话版本：

- `RpcClient` 是唯一的生命周期所有者。
- `VesClient` 只是构建在 `RpcClient` 之上的薄封装。
- `AnyCall` 是 VES 侧唯一允许保留的业务降级路径。
- facade 不应继续扩出额外的测试语义接口；生命周期真源始终是统一快照和事件报告。

## 职责归属

### `RpcClient`

`RpcClient` 负责：

- 生命周期状态迁移
- `Cooldown` / `Recovering` 窗口
- idle close 处理
- 引擎死亡处理
- 外部健康信号触发的恢复请求
- 通过 `RecoveryRuntimeSnapshot` 和 `RecoveryEventReport` 输出结构化 DFX
- 带恢复感知的调用等待

如果恢复语义要改，入口应该先落在 `RpcClient`。

### `VesClient`

`VesClient` 只负责：

- 类型化 request / reply 的编解码桥接
- VES 默认策略装配
- 在 `memrpc` 主路径和 `AnyCall` 降级路径之间做选择
- 向 VES 调用方暴露结构化恢复状态

`VesClient` 不应再维护自己的 cooldown 状态机或恢复重试循环。

### `VesBootstrapChannel`

`VesBootstrapChannel` 只负责：

- 持有当前 bootstrap control
- 在 control 死亡时让旧引用失效并上报 `EngineDeath`
- 在 `OpenSession()` 或显式 `CurrentControl()` 取用时按需刷新 control

`VesBootstrapChannel` 不应在死亡通知回调里偷偷预热下一份 control；死亡事件和 control 刷新是两个独立动作。

## 观测视图

对测试、示例或兼容旧代码，允许直接消费统一结构化观测；但它们不能被当成状态真源。

统一观测来源只有：

- `RecoveryRuntimeSnapshot`
- `RecoveryEventReport`

不要再新增 `engineDied_`、`cooldownActive_` 这类粘滞布尔量，或每个封装层自己维护的恢复标志。测试如果要看更细的触发过程，优先在测试侧直接挂 `RpcClient` 回调，而不是扩 public API。

这里也包括 bootstrap 层：

- `EngineDeath` 事件只说明旧 session / 旧 control 已失效
- 它不等价于“新 control 已经就绪”
- 是否刷新出新的 control，应由后续显式取用点决定

## 业务降级路径

VES 侧只允许一条业务旁路：

- `AnyCall`

它只能在 `memrpc` 主路径不适合时作为传输降级使用，例如请求过大。

`AnyCall` 不能做下面这些事：

- 变成第二个生命周期所有者
- 自己合成生命周期状态迁移
- 重新定义 `memrpc` 的恢复语义
- 暗示 `memrpc` 已经回到 `Active`

## 建议阅读顺序

排查恢复行为时，建议按下面顺序读代码：

1. `memrpc/include/memrpc/client/rpc_client.h`
2. `memrpc/src/client/rpc_client.cpp`
3. `virus_executor_service/src/client/ves_client.cpp`

看测试时，优先看那些先断言结构化状态的用例：

- `memrpc/tests/rpc_client_external_recovery_test.cpp`
- `memrpc/tests/rpc_client_idle_callback_test.cpp`
- `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

## 实用判断规则

如果你准备增加一段恢复逻辑，但不确定应该放在哪里：

- 如果它改变生命周期，就放进 `RpcClient`
- 如果它改变 VES 请求编码，或者改变主路径/降级路径选择，就放进 `VesClient`
- 如果它只是为了让旧测试或示例更容易读状态，就从统一 snapshot/report 派生，而不是再加新状态
