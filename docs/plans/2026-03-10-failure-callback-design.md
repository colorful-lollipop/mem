# MemRPC 失败回调设计（框架级）

日期：2026-03-10

## 背景

当前 `memrpc` 只有事件回调（`RpcEventCallback`），失败信息主要通过调用者等待 `RpcFuture` / `InvokeSync` 返回的 `StatusCode` 获知。应用若想在全局维度聚合失败、做重启策略，需要在所有调用点自行包装，容易漏掉提交阶段失败或会话级断链。

## 目标

- 框架提供“每次失败必达”的回调机制，覆盖**框架级失败**（超时、断链、协议错误等）。
- 业务层自行做失败聚合、阈值判断、重启策略。
- 不引入业务语义（`engine_code/detail_code`）和 payload。

## 非目标

- 不在框架内做失败阈值判断或重启策略。
- 不承诺业务层错误码语义。
- 不改变现有同步/异步调用语义。

## 方案概述

在 `RpcClient` 内新增失败回调接口与失败元数据结构；在请求被判定为失败的**所有路径**触发回调。回调只包含框架级元数据，不包含 payload，避免敏感数据泄露。

## API 设计

新增结构体与回调类型：

- `enum class FailureStage { Admission, Response, Session };`
- `struct RpcFailure`：
  - `StatusCode status`（非 `Ok`）
  - `Opcode opcode`
  - `Priority priority`
  - `uint32_t flags`
  - `uint32_t admission_timeout_ms`
  - `uint32_t queue_timeout_ms`
  - `uint32_t exec_timeout_ms`
  - `uint64_t request_id`
  - `uint64_t session_id`
  - `uint32_t monotonic_ms`（失败发生的单调时钟毫秒）
  - `FailureStage stage`

新增回调：

- `using RpcFailureCallback = std::function<void(const RpcFailure&)>;`
- `void RpcClient::SetFailureCallback(RpcFailureCallback cb);`

> 位置建议：
> - `RpcFailure`、`RpcFailureCallback` 放在 `memrpc/client/rpc_client.h`（与 `RpcClient` 紧密相关）；
> - 若后续需要在其他组件复用，可迁到 `memrpc/core/types.h`。

## 回调触发点

1. **提交阶段（Admission）**
   - `QueueTimeout`、`QueueFull`、`PeerDisconnected`、`EngineInternalError` 等提交失败路径。
2. **响应阶段（Response）**
   - 收到 reply 但 `status != Ok`。
   - `ProtocolMismatch` 触发回调，并进入会话断链清理。
3. **会话失败阶段（Session）**
   - `HandleEngineDeath` 触发时，对仍在 pending 的请求统一以 `PeerDisconnected` 回调。

回调只针对失败（`StatusCode != Ok`），成功路径不触发回调。

## 线程模型与约束

- 回调在**触发失败的内部线程**执行（提交线程或响应线程）。
- 回调必须轻量、不可阻塞；业务若需复杂处理应自行派发到工作线程。
- 允许多线程并发回调（提交线程与响应线程同时触发）。

## 应用层聚合建议（DFX）

- 业务层只关注执行超时（`ExecTimeout`）或致命错误（`PeerDisconnected`、`ProtocolMismatch`）。
- 排队超时可在业务层忽略，建议将 `queue_timeout_ms = 0` 禁用排队超时。
- 建议滑动窗口统计，例如“1 分钟 3 次 `ExecTimeout`”触发重启；
- 对致命错误可更快触发重启，但需加入冷却时间/指数退避，避免重启风暴。
- 可按 `Opcode` 分桶统计，避免单个慢请求影响全局策略。

## 兼容性

- 不改变现有调用语义和结构体；未设置回调时无行为变化。
- 回调只携带框架元数据，不破坏隐私边界。

## 风险与缓解

- **回调阻塞**：明确要求回调轻量，并建议业务层派发。
- **多线程竞态**：文档要求回调实现线程安全。

## 测试建议

- 单元测试：提交阶段失败触发回调；响应阶段失败触发回调。
- 会话断链：`PeerDisconnected` 统一回调。
- 成功路径不触发回调。

