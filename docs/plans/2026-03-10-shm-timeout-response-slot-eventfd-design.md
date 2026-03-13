# 共享内存超时语义 + Response Slot 生命周期 + 去轮询设计

## 目标

- 修正三段超时（admission / queue / exec）语义，使同步/异步一致且可解释。
- 明确 response slot 生命周期：发布后只由 client 回收，杜绝“发布后被 server 释放”的悬空风险。
- 去除 response writer 的固定轮询，改成 eventfd 驱动的资源等待。
- payload 固定为 4K（编译期），总共享内存目标 4MB，停机升级即可。

## 约束与前提

- 单 client / 单 server / 单 session 模型保持不变。
- 共享内存布局允许修改（停机升级）。
- 仍使用 3 条 ring（高优 / 普通 / 响应）+ request slot + response slot。
- 已有 5 个 eventfd（请求/响应通知 + request/response credit）保持语义分离。
- 不新增 StatusCode，超时返回沿用 `QueueTimeout` / `ExecTimeout`。

## 方案概览

### 1) 三段超时语义（admission / queue / exec）

**统一语义：**

- `admission_timeout_ms`：只覆盖 client 侧“入队/写 ring”的等待。
  - 到期仍未成功入 ring：返回 `QueueTimeout`。
  - 不落请求，server 不可见。
- `queue_timeout_ms`：只覆盖 server 端排队等待进入 worker 的时间。
  - 超时由 server 判定并回包 `QueueTimeout`。
- `exec_timeout_ms`：只覆盖 handler 实际执行耗时。
  - server 执行完成后若超时，回包折叠为 `ExecTimeout`（不强杀 handler）。

**同步调用规则：**

- 若三段均为 0：无限等待。
- 否则总等待预算 = `admission + queue + exec`，同步等待只依赖这一预算。
- 本地等待超时仅返回 `QueueTimeout`，不折叠为 `PeerDisconnected`。

### 2) Response Slot 生命周期与通知失败语义

**生命周期规则：**

- server 在写入 response slot 并成功发布 ring entry 后，不再回收 slot。
- response slot 仅由 client 在消费后回收。

**通知失败语义：**

- `respEventFd` 写失败不触发 slot 回收。
- 允许记录错误并标记 session health（必要时置为 broken），但 slot 仍由 client 回收。

**一致性校验：**

- client 在消费时校验 `response_slot.runtime.request_id` 与 ring entry 的 `request_id` 一致。
- 不一致视为 `ProtocolMismatch`，不释放 slot，并触发会话失败路径。

### 3) 去轮询：response writer 事件化等待

**核心规则：**

- 资源不足时只等待 `respCreditEventFd`，不再固定 `sleep_for` / 周期性轮询。
- 等待基于 “一次阻塞 + deadline”：
  - 先 `TryReserve` / `TryPush`。
  - 失败则 `poll/ppoll` 等待 credit，超时则返回失败。

**credit 写入时机：**

- client response loop 在以下动作后写 `respCreditEventFd`：
  - 成功 `PopResponse()`（response ring 变非满）
  - 释放 response slot（slot pool 变非空）

### 4) 固定 4K payload 与 4MB 预算

- `kDefaultMaxRequestBytes = 4096`  
- `kDefaultMaxResponseBytes = 4096`
- 固定编译期常量，不做运行时协商。
- 共享内存布局在 bootstrap 中使用固定参数计算。
- 推荐配置：
  - `high_ring_size = 64`
  - `normal_ring_size = 256`
  - `response_ring_size = 256`
  - `slot_count = 256`
  - 该配置下总 size 约 2.2MB，明显低于 4MB 上限。
- 若后续需要更高并发，可在不改变 payload 上限的情况下按固定编译配置调整 slot/ring 数量。

## 数据流与组件变化

### Client

- admission 失败时等待 `reqCreditEventFd`，等待预算由 `admission_timeout_ms` 控制。
- response loop 消费 reply/event 后：
  - 释放 response slot，并写 `respCreditEventFd`
  - 若为 reply，释放 request slot 并写 `reqCreditEventFd`
- 校验 response slot 的 `request_id` 与 ring entry 一致性。

### Server

- response writer：资源不足时仅等待 `respCreditEventFd`，不再轮询。
- response slot 发布后不再释放；只有 client 回收。
- `respEventFd` 写失败仅影响 session 健康度，不回收 slot。

## 错误处理原则

- 不新增状态码，语义通过超时/协议错误路径体现。
- eventfd 失败属于会话级错误，允许进入 broken 流程。
- 所有 release 路径必须保证“未发布前可释放，发布后只由 client 释放”。

## 测试策略

1. **三段超时**
   - admission 超时必须返回 `QueueTimeout`，且 server 不可见该请求。
   - queue/exec 超时与现有 server 行为一致。
2. **response slot 生命周期**
   - eventfd 写失败时 slot 不被 server 回收，client 仍能消费。
   - slot request_id 校验失败触发 `ProtocolMismatch`。
3. **去轮询**
   - response writer 在资源不足时阻塞等待 credit，释放资源后立刻继续。
   - CPU 使用不出现 1ms 轮询抖动。

## 迁移与兼容性

- 停机升级，允许协议变更与共享内存布局更新。
- 建议 `kProtocolVersion` 自增，防止旧二进制误连新布局。

## 结论

本设计在保持整体模型不变的前提下：

- 修正超时语义的确定性与一致性
- 消除 response slot 的发布后释放风险
- 去除 response writer 轮询抖动
- 统一 payload 上限为 4K，满足 4MB 预算

适合作为工业化收敛阶段的核心修正方案。
