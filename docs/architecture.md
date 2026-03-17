# MemRPC 架构说明

## 当前模型

`MemRpc` 当前是一个面向小包共享内存 RPC 的基础层，主线只覆盖：

- 一块共享内存
- 两条请求 ring
  - high priority
  - normal priority
- 一条 response ring
- 五个 `eventfd`
  - `high_req_eventfd`
  - `normal_req_eventfd`
  - `resp_eventfd`
  - `req_credit_eventfd`
  - `resp_credit_eventfd`

协议已经收敛为固定 entry 设计：

- `RequestRingEntry` 固定 `512B`
- `ResponseRingEntry` 固定 `512B`
- 请求正文直接内嵌在 request entry
- 响应和事件正文直接内嵌在 response entry
- 不再有 request slot、response slot、slot pool 或 slot 生命周期

当前默认 inline 上限来自协议头部布局：

- request payload：`480B`
- response payload：`472B`

对应实现见 [protocol.h](/root/mem/memrpc/include/memrpc/core/protocol.h#L10)。

## 请求与响应语义

小请求走共享内存主路径：

1. 客户端编码请求。
2. 如果 payload 不超过 `DEFAULT_MAX_REQUEST_BYTES`，直接写入 request ring entry。
3. 服务端从 entry 构造 `RpcServerCall`，执行业务 handler。
4. 如果响应不超过 `DEFAULT_MAX_RESPONSE_BYTES`，直接写入 response ring entry。
5. 客户端按 `requestId` 完成 pending 请求。

超限语义保持明确且保守：

- 请求超限：由应用层在发起前决定是否改走别的通道。
- 响应超限：直接返回 `PayloadTooLarge`。
- 事件超限：`PublishEvent()` 直接返回 `PayloadTooLarge`。
- 不做“memrpc 执行后再自动切 AnyCall”的补救路径，避免重复执行和副作用不确定性。

## VES 集成方式

`virus_executor_service` 采用“共享内存主路径 + 控制面兜底”的组合：

- `VesClient` 对外保留同步 typed API。
- 编码后的请求如果不超过 `DEFAULT_MAX_REQUEST_BYTES`，走 `memrpc` 同步调用。
- 编码后的请求如果超过上限，直接走同步 `AnyCall`。
- `AnyCall` 复用原有 opcode/handler 语义，不引入新的业务协议层。

对应实现见：

- [ves_client.cpp](/root/mem/virus_executor_service/src/client/ves_client.cpp#L104)
- [ves_control_interface.h](/root/mem/virus_executor_service/include/transport/ves_control_interface.h#L50)

这意味着当前对外设计重点是：

- 小包、高频路径优先走共享内存
- 大请求显式走控制面
- 大响应不兜底

## 并发与调度

当前并发模型仍然允许内部异步实现，但不再把“异步对外接口”当成主线设计目标。

服务端：

- dispatcher 优先消费 high ring
- high/normal 请求可投递到不同执行线程池
- worker 产出 completion
- response writer 统一写 response ring
- ring 满时通过 credit `eventfd` 等待恢复，不做固定间隔轮询

客户端：

- `RpcClient` 仍保留 async 基础能力
- 同步调用仍是框架内建能力
- `VesClient` 这类业务 facade 对外只暴露同步接口
- pending 以 `requestId` 跟踪，不再和 slot 绑定

## 恢复与健康检查

恢复边界保持收口：

- `IBootstrapChannel::CheckHealth()` 只负责通用健康结果
- `VesControlProxy` 保留业务 heartbeat 协议和健康快照
- `RpcClient` watchdog 统一处理 timeout、health failure、engine death、idle close
- `VesClient` 只做策略装配和业务侧日志/订阅

恢复语义保持保守：

- 旧 session 上的等待请求在会话失效后立即失败
- `RpcClient` 是唯一 lifecycle owner，timeout / engine death / external health / idle close / manual shutdown 都先进入统一状态机
- 下一次调用自动尝试重建 session，但仅限非终态路径
- 框架不会自动重放可能已经被旧服务端看到的请求

### 统一 client 生命周期

`RpcClient` 现在显式维护 `ClientLifecycleState`：

- `Uninitialized`
- `Active`
- `Cooldown`
- `IdleClosed`
- `Recovering`
- `Closed`

其中：

- `Shutdown()` 只会把 client 带到 `Closed`，这是终态；后续不会再接受 recovery signal，也不会再自动 reopen
- idle policy 触发的 `CloseSession` 只会进入 `IdleClosed`；它关闭当前 session，但 client 仍可复用
- `Cooldown` 和 `Recovering` 是框架内部恢复态，业务侧只消费 snapshot/report，不直接改状态
- `DemandReconnect` 只发生在 `IdleClosed` 之后的下一次真实调用

### 合法转换

- `Uninitialized -> Active`
- `Active -> Cooldown`
  由 `ExecTimeout`、`EngineDeath`、`ExternalHealthSignal` 等恢复触发，且带 delay
- `Active -> Recovering`
  由立即恢复路径触发，或 cooldown 结束后开始 reopen
- `Active -> IdleClosed`
  仅由 idle policy 驱动的 `CloseSession`
- `IdleClosed -> Recovering -> Active`
  仅由 `DemandReconnect` 触发
- `* -> Closed`
  仅由手动 `Shutdown()`

### DFX 与观测面

框架统一暴露两类观测对象：

- `RecoveryRuntimeSnapshot`
  提供当前 lifecycle、最近 trigger、最近 recovery action、cooldown 剩余时间、session ids、manual-terminal 标记
- `RecoveryEventReport`
  提供一次状态迁移的前后状态、trigger、action、cooldown 计划和 session 关联信息

`VesClient` 不再维护独立的 engine-dead 布尔状态或自定义 cooldown 状态机；它只负责配置 recovery policy，并缓存/消费 `RpcClient` 给出的统一 snapshot/report。

## 适用范围

当前主线方案的目标很明确：

- 用 `memrpc` 覆盖绝大多数正常小包场景
- 用 `AnyCall` 或其他 IPC 兜底少量超大请求
- 用显式失败替代“大包自动回拉”这类复杂补救链路

如果某个业务天然以大 payload 为主，它就不应该强行挤进当前 `memrpc` 主路径。
