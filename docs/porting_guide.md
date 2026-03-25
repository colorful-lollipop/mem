# 迁移说明

## 核心原则

迁移新业务时，先按当前主线边界设计，不要再沿用旧的 slot 方案或“对外 async-first”的包装习惯。

当前推荐模型是：

- 小请求走 `memrpc`
- 大请求走同步 `AnyCall` 或其他 IPC
- 业务对外接口默认保持同步
- 框架内部是否异步，由实现自行决定
- 生命周期检测、恢复决策、恢复等待统一留在 `RpcClient`

## 框架与应用的边界

框架层只负责通用通信能力：

- 请求提交
- 响应完成
- 无头事件
- 会话恢复
- 背压和健康检查

应用层自己负责：

- 请求/响应类型
- codec
- 业务封装层
- handler 注册
- 是否需要保留单条业务兜底旁路

框架公开头路径保持不变：

- `memrpc/core/*`
- `memrpc/client/*`
- `memrpc/server/*`

## 当前推荐结构

建议应用按下面方式组织：

- `<app>/include`
  - request / reply 类型
  - codec
  - 同步封装层
  - service 接口
- `<app>/src`
  - 封装层实现
  - service / handler
  - transport / bootstrap
- `<app>/tests`
  - unit
  - integration
  - stress / dt / fuzz

如果内部确实需要更高吞吐或并行流水线，可以在应用内部保留 async client，但不要把它当成默认对外接口。

## 新增一个 RPC 的最小步骤

新增一条业务调用时，通常只需要：

1. 定义 request / reply 类型。
2. 增加 codec。
3. 在应用封装层里加一个同步薄方法。
4. 在服务端注册对应 handler。
5. 判断该请求是否可能超过 `DEFAULT_MAX_REQUEST_BYTES`。
6. 如果可能超限，明确决定：
   - 改走同步 `AnyCall`
   - 或直接走其他 IPC

不需要再改：

- 共享内存 layout 主干
- slot 生命周期
- slot 回收逻辑
- 自动大包补救链路

## Payload 设计约束

当前主线不是“大包万能通道”。

- request entry 固定 `512B`
- response entry 固定 `512B`
- request inline 上限约 `480B`
- response inline 上限约 `472B`

因此迁移时要尽早区分两类调用：

- 热路径小包调用：优先放进 `memrpc`
- 冷路径或超大调用：直接走控制面兜底

不要把“大请求走共享内存，失败后再补拉”或“业务封装层自己再套一层恢复循环”的复杂链路重新带回来。

## VES 样板

当前仓库里的主线样板是 `virus_executor_service`：

- `ves` 展示业务同步封装层 + 单条 `AnyCall` 兜底
- `testkit` 展示测试、压测和故障注入能力

其中 `VesClient` 的当前行为是：

- 先编码业务请求
- 小于等于 inline 上限时走 `memrpc`
- 大于 inline 上限时直接走同步 `AnyCall`
- 如果走 `memrpc`，恢复等待与重连判断全部复用 `RpcClient::RetryUntilRecoverySettles(...)`
- `EngineDeath` 只负责让旧 session / control 失效；下一次真正需要 control 时才按需刷新

这比“对外 async client + 多级大包补救”更符合当前主线。

## 恢复语义

恢复策略继续保持保守，并且只在 `RpcClient` 内部闭环：

- 子进程死亡后，旧 session 上的等待请求立即失败
- 后续调用再尝试建新 session
- 框架不会自动重放可能已经被旧服务端看到的请求
- `VesClient` 不再维护自己的恢复状态机或本地等待循环
- 不要在 bootstrap 死亡通知里混入“顺手 reload control”这类隐式恢复动作

业务层仍然保留最终决策权，例如是否重扫、是否丢弃结果、是否改走其他 IPC；但这属于业务语义，不属于框架恢复路径。
