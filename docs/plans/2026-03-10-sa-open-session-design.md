# SA OpenSession/CloseSession 设计

## 背景与问题
当前 demo 路径使用 "client 先创建 shm/eventfd -> fork 子进程" 的方式启动 server，导致启动语义与 SA 模式偏离。对外接口仍是 `StartEngine/Connect`，语义分散且容易被误用。

目标是对外提供 **唯一的 SA IPC 启动接口**，让 client 与 server 独立启动，server 作为资源所有者，client 通过 IPC 拿到 `BootstrapHandles` 并 attach。

## 目标
- 对外仅暴露 `OpenSession()` / `CloseSession()`，启动语义收敛。
- Server 端拥有并创建 shm/eventfd，client 仅连接。
- 会话为单一实例，`OpenSession()` 幂等。
- `CloseSession()` 释放资源并允许 server 退出或进入空闲。

## 非目标
- 多 client 并发共享同一 session。
- 复杂的 session 配置协商（配置编译期确定）。

## 核心接口
对外 SA IPC 只暴露：
- `StatusCode OpenSession(BootstrapHandles* handles)`
- `StatusCode CloseSession()`

`IBootstrapChannel` 同步收敛到上述接口，并保留 `SetEngineDeathCallback()`。

内部实现可以保留分步逻辑，但不对外暴露：
- `EnsureServerReady()`（替代 `StartEngine`）
- `AcquireSessionHandles()`（替代 `Connect`）

## BootstrapHandles
继续包含：
- `shmFd`
- `highReqEventFd` / `normalReqEventFd` / `respEventFd`
- `reqCreditEventFd` / `respCreditEventFd`
- `protocol_version` / `session_id`

## 数据流
- Client: `OpenSession()` -> `Session::Attach()` -> 启动 submitter/dispatcher。
- Server: `OpenSession()` 触发懒创建 shm/eventfd，注册并返回 handles；`RpcServer` 使用同一 handles `Start()`。
- `CloseSession()` 释放 shm/eventfd，server 可退出或回到空闲。

## 崩溃与恢复
- Server crash 不会导致 client 崩溃，但会导致请求无响应或超时。
- SA death callback 触发后，client 立即 fail pending 并上报应用。
- 重启/退避策略由应用决策；下一次 `OpenSession()` 返回新 session。

## 错误处理
- `OpenSession()` 失败直接返回 `StatusCode`，不进入 `Attach()`。
- `OpenSession()` 幂等：已有会话时返回同一 handles。
- `CloseSession()` 幂等：无会话时返回 `Ok`。
- `Session::Attach()` 继续校验 `protocol_version/session_id`。

## 测试与迁移
- 单测：`OpenSession/CloseSession` 成功、失败、幂等。
- 集成：Open -> RPC -> Close -> 再 Open -> RPC。
- Demo：替换为 SA mock，模拟 server 创建 session，client 通过 IPC 拿 handles。
