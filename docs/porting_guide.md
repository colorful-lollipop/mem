# 迁移说明

## 目标

框架层只提供通用跨进程 RPC 能力：

- 发请求
- 收响应
- 接收无头事件
- 会话恢复

应用层自己兼容自己的旧接口。

这意味着后续像 VPS 这类复杂业务，不应该再要求 `memrpc` 内置专用 `EngineClient/EngineServer`。更合理的做法是：

- 应用层实现异步 client
- 应用层再包一层同步 facade

## 推荐迁移结构

建议应用层按这个形态组织：

- `<app>/include`
  - 请求/响应类型
  - codec
  - client facade
  - service/handler 注册接口
- `<app>/src`
  - client
  - engine/service
  - transport/bootstrap/registry
- `<app>/tests`
  - unit
  - integration
  - stress / dt / fuzz

框架层只保留：

- `RpcClient`
- `RpcFuture`
- `RpcServer`
- `BootstrapHandles`
- session / ring / shared memory / recovery

当前响应模型也已经固定为：

- 请求写入共享内存 slot
- 响应和事件统一走响应队列
- 单条响应/事件 payload 上限为 `1KB`

框架头文件使用也统一为：

- `memrpc/core/*`
- `memrpc/client/*`
- `memrpc/server/*`

## 当前样板

当前仓库里的主线样板是：

- `virus_executor_service`

它同时展示了两类协议如何共用同一条 `memrpc` 通道：

- 业务层：`ves`
- 测试层：`testkit`

其中 `testkit` 继续保留了 `Echo/Add/Sleep`、故障注入、吞吐/延迟/DT/stress/fuzz 这些测试型能力；`ves` 负责业务调用。后续迁移新应用时，建议直接照这个模式组织，而不是把业务细节塞回框架层。

新的业务接入应直接放到 `<app>/include|src|tests`。

## 事件模型

如果应用层需要额外通知，不需要再单独发明一条通知通道。

当前框架已经支持：

- 单响应队列
- 单 `resp_eventfd`
- `Reply/Event` 双消息类型

应用层可以：

- 用普通 RPC 做请求-响应
- 用 `RpcServer::PublishEvent()` 发无头事件
- 在客户端用 `RpcClient::SetEventCallback()` 接收
- 再在应用层本地做 listener 分发

框架不会把事件绑定到具体请求，也不会管理应用层 listener 对象。

## 新增一个 RPC 的最小改动

在当前结构下，新增一个函数时，目标只改这些地方：

- 定义 request/response 类型
- 手写 codec
- 应用层 client/facade 增一个薄方法
- 服务端 service 注册一个 handler
- 如果旧接口需要同步语义，再包一层 facade

不需要再改：

- 共享内存布局主干
- 请求/响应队列主干
- 优先级调度
- 会话恢复逻辑

## 恢复语义

当前恢复策略保持保守：

- 子进程死亡回调到来时，旧 session 上的等待请求立即失败
- 下一次调用自动尝试重建 session
- 框架不会自动重放可能已经被旧子进程看到的请求

所以应用层仍应保留最终业务决策权，例如是否重扫、是否重建更高层状态。

## Linux 与鸿蒙

- Linux 开发态可以用 demo bootstrap 或 fake SA
- HarmonyOS 正式环境中，应由 `init` 拉起子进程
- 客户端通过 `GetSystemAbility` / `LoadSystemAbility` 获取服务和句柄

也就是说，迁移到鸿蒙时重点是平台 bootstrap 适配，而不是重写 `MemRpc` 通信核心。
