# MemRPC 架构说明

`MemRpc` 是一套基于共享内存和 `eventfd` 的通用跨进程 RPC 框架。当前主线构建只关注两层：

- 框架层：`memrpc`
- 最小应用样板：`apps/minirpc`

复杂业务层例如 VPS 不进入主库构建，但保留独立 codec 和单测，用来验证小 payload 降拷贝路径。

仓库中的 `legacy/` 目录仅保留历史参考实现，不参与当前主线构建和测试。

框架公开头也只保留分层路径：

- `memrpc/core/*`
- `memrpc/client/*`
- `memrpc/server/*`

## 当前主线

当前会话模型包含：

- 一块共享内存
- 一条高优请求队列
- 一条普通请求队列
- 一条响应队列
- 五个 `eventfd`
  - `high_req_eventfd`
  - `normal_req_eventfd`
  - `resp_eventfd`
  - `req_credit_eventfd`
  - `resp_credit_eventfd`

共享内存里的 slot 现在分成两类：

- `request slot pool`
- `response slot pool`

请求和响应都采用 `ring + slot`：

- request ring 只放 `request_id/slot_index/opcode/...`
- response ring 只放 `request_id/slot_index/message_kind/...`
- 正文分别放在 request/response slot 中

默认 session 配置：

- request 上限：`4KB`
- response 上限：`4KB`
  - 该上限固定用于 request/response slot payload
  - 运行时配置不能超过这个值

## 响应路径模型

response ring 统一承载两类消息：

- `Reply`
- `Event`

二者共用同一个 `resp_eventfd`。框架不会再维护独立的 notify ring 或额外通知 fd。

`Reply` 用于普通 RPC 回包：

- 通过 `request_id` 命中 pending request
- ring entry 只携带状态码和 `response_slot_index`
- 正文在 `response slot` 中
- 唤醒对应等待者

`Event` 用于无头广播事件：

- 不依赖 `request_id`
- 带 `event_domain`、`event_type`、`flags`
- 同样通过 `response slot` 携带 payload
- 客户端分发线程收到后直接交给应用层事件回调

这样做的目标是：

- 保持底层通道简单
- 避免轮询额外事件 fd
- 同时给应用层保留异步事件能力

## 响应 slot 生命周期

response slot 的生命周期必须有确定性语义：

- server 写入 payload 后才会发布 response ring entry
- client 只有在成功读到 ring entry 后才会消费对应 slot
- client 校验 ring entry 与 slot 内 `request_id` 一致，不一致视为 `ProtocolMismatch`
- `resp_eventfd` 通知失败时，slot 不能被提前复用，视为会话异常并触发 session 终止/清理

这保证了“写入 ring 成功但通知失败”的路径不会产生悬空或重复释放。

## 唤醒模型

当前 `eventfd` 分成两类：

- 消息到达通知
  - `high_req_eventfd`
  - `normal_req_eventfd`
  - `resp_eventfd`
- 资源恢复通知
  - `req_credit_eventfd`
  - `resp_credit_eventfd`

消息到达 fd 采用边沿语义：

- request ring 只在 `empty -> non-empty` 时通知一次
- response ring 只在 `empty -> non-empty` 时通知一次
- 被唤醒的一侧需要主动把 ring 尽量 drain 到空

credit fd 采用“资源重新可写”语义：

- `req_credit_eventfd`
  - `server dispatcher` 在 request ring 从满变为非满时通知
  - `client response loop` 在 request slot 从 `0 -> >0` 时通知
- `resp_credit_eventfd`
  - `client response loop` 在 response ring 从满变为非满时通知
  - `client response loop` 在 response slot 从 `0 -> >0` 时通知

这些 fd 都只是 wakeup hint，不是资源计数真相。真正的事实来源仍然是：

- shared ring cursor
- request/response slot pool 状态

这样做的目的有两点：

- 去掉 request/response 两侧的 `1ms` 轮询重试
- 在小 payload 高频场景下减少 `eventfd write/read + poll` 的固定 syscall 开销

## 超时语义

同步调用采用三段式超时模型，所有阶段共享一个总 deadline：

- admission queue wait：等待 request ring/slot 资源恢复
- execution：服务端 handler 实际执行
- response wait：等待 response ring/slot 可写 + client 收到 reply

具体语义：

- admission timeout 返回 `QueueTimeout`
- execution timeout 由 `exec_timeout_ms` 控制
- response wait 使用剩余时间，不再叠加额外 padding

## 框架接口

框架只提供通用能力，不再自带业务兼容层。

客户端公共接口：

- `RpcClient::Init()`
- `RpcClient::InvokeAsync()`
- `RpcClient::InvokeSync()`
- `RpcFuture::WaitAndTake()`
- `RpcClient::SetEventCallback()`

服务端公共接口：

- `RpcServer::RegisterHandler()`
- `RpcServer::PublishEvent()`
- `RpcServer::Start()`
- `RpcServer::Stop()`

框架层不再内置 `EngineClient/EngineServer` 这类业务兼容接口。应用如果需要兼容旧同步调用，应在自己的目录下实现同步 facade。

## 调度与并发

服务端当前模型：

- dispatcher 优先消费高优请求队列
- 高优和普通请求分别投递到独立线程池
- worker 只产生 completion，不直接写共享 response ring
- response writer 线程统一写共享 response ring
- response writer 在 response ring/slot 资源不足时等待 `resp_credit_eventfd`
- credit 等待为单次 `poll`，使用完整剩余时间，不做固定间隔轮询
- 高优请求允许长期压制普通请求

客户端当前模型：

- 内部以异步事务为底层模型
- request 先进入本地提交队列，再由 tx thread 写共享 request ring
- submitter 在 request ring/slot 资源不足时等待 `req_credit_eventfd`
- 同步调用只是 `InvokeAsync + deadline wait` 的薄包装
- 一个 `RpcClient` 对应一条响应分发线程
- `InvokeAsync(RpcCall&&)` 会把编码后的 payload move 进提交队列，避免在 client API 边界再复制一份
- `RpcFuture::WaitAndTake()` 会把内部 `RpcReply` move 给调用方，避免小包回包在 future 出口再复制一份

需要注意：

- 当前框架支持一个客户端实例内的多并发请求
- 不支持多个 `RpcClient` 实例共享同一个响应队列并同时消费同一会话
- `RpcClientRuntimeStats` / `RpcServerRuntimeStats` 暴露了当前是否在等待 credit，便于测试和排障

## 日志规范

框架层当前提供一个很薄的日志门面：

- 头文件固定为 `virus_protection_service_log.h`
- 宏风格为 `HLOGD/HLOGI/HLOGW/HLOGE`
- 同时兼容 `HILOGD/HILOGI/HILOGW/HILOGE` 别名
- 支持 `%{public}` / `%{private}` 这类鸿蒙风格格式串

当前实现只做最小兼容：先剥掉可见性前缀，再按普通 `printf` 风格格式化。

日志只建议打在必要路径：

- bootstrap 初始化失败
- session 死亡与恢复
- 协议异常
- 事件发布失败

不要在高频正常路径里铺满日志，避免影响核心通信层的开销和可读性。

## 事件边界

事件是可选能力，不是主线调用方式。

框架只负责：

- 传输 `event_domain/event_type/flags/payload`
- 在客户端把事件交给回调

框架不负责：

- 解释业务事件内容
- 将事件绑定到某次具体 RPC
- 管理应用层 listener 生命周期

这些都留给应用层处理。

## MiniRpc 样板

`MiniRpc` 是最小应用样板，用来验证框架本身的泛用性。

当前只保留三个基础 RPC：

- `Echo`
- `Add`
- `Sleep`

它的作用是：

- 验证 request/response 主路径
- 验证同步 facade 和异步 client 的组合方式
- 验证优先级和超时语义
- 验证 request/reply 全量 view decode，服务端默认 view，保留 owning 用于对比/兼容
- 通过吞吐量基线测试持续监控 `Echo/Add/Sleep` 的 ops/sec，回退超过 10% 视为失败

`MiniRpc` 不承担复杂业务兼容职责，也不强依赖事件模型。

## 小包降拷贝现状

当前已经收紧到下面这条边界：

- client API 边界：
  - 编码后的 `std::vector<uint8_t>` 可以 move 进 `RpcCall`
  - `RpcClient::InvokeAsync(RpcCall&&)` 会继续 move 到 submit queue
- server handler 边界：
  - `RpcServerCall.payload` 是 `PayloadView`
  - handler 可以直接基于共享 request slot 做只读或 view-based decode
- client 回包边界：
  - `RpcFuture::WaitAndTake()` 可以把 `RpcReply` move 给调用方
- codec 边界：
  - `ByteReader` 支持 `ReadStringView()` / `ReadBytesView()`
  - `MiniRpc` request/reply 已全量支持 view decode，并保留 owning 模式
  - VPS codec 的外层 envelope 已改成在原始字节流上推进，不再切中间 `vector`

仍然保留的 owning 边界：

- request submitter 仍然要把 payload `memcpy` 到 request slot
- response loop 仍然要把 response slot materialize 成 `RpcReply.payload`
- 大对象业务结构默认仍保留 owning decode，view codec 只覆盖热点路径
- `MiniRpc` handler 仍保留 owning decode 分支，便于兼容和性能对比

## 恢复语义

当前恢复策略保持保守：

- 子进程死亡回调到来时，当前 session 立即失效
- 旧 session 上等待中的请求立即返回 `PeerDisconnected`
- 后续新的调用会自动尝试 `OpenSession()`
- 可能已经被旧子进程看到的请求不会自动重放

这条边界是刻意保守的，目的是先保证正确性和可控性。进程 death / owner death / session 失效感知由 bootstrap 层负责，shared ring 本身不再依赖 robust ring mutex 做恢复。

## 平台分层

传输层和平台拉起解耦：

- Linux 开发态：`fork()` demo 或 fake SA bootstrap
- HarmonyOS 正式环境：`init` 拉起进程，`GetSystemAbility` / `LoadSystemAbility` 完成句柄交换

迁移到鸿蒙时，主要替换 bootstrap 层，不需要推翻共享内存通信核心。
