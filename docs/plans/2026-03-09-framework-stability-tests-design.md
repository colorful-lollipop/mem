# 框架核心稳定性测试补强设计

## 目标

在不扩展新功能的前提下，补强 `memrpc` 主线的核心稳定性测试，优先覆盖恢复语义、队列与上限边界，以及高并发正确性。

## 范围

本轮只关注框架层：

- `tests/memrpc/*`
- `src/core/*`
- `src/client/rpc_client.cpp`
- `src/server/rpc_server.cpp`
- `src/bootstrap/*`

不扩展应用层功能，不新增 VPS 兼容逻辑，不做 benchmark。

## 优先级

### 1. 恢复语义

优先验证：

- 子进程死亡回调后，旧 session 上的 pending request 立即失败
- 下一次调用能自动重建 session
- 新 session 不复用旧 `session_id`
- 旧 session 的响应或事件不会污染新 session

### 2. 队列与上限边界

补强：

- 请求队列满时返回 `QueueFull`
- 请求 payload 超上限时快速失败
- 响应 / 事件 payload 超配置上限时被拒绝

### 3. 高并发正确性

补强：

- 多线程并发 `InvokeAsync()` 不丢结果
- 高优请求在普通积压下优先完成
- `Reply/Event` 混发时不串包、不误命中 pending map

## 设计原则

- 尽量复用现有测试文件，不新开大量测试文件
- 单元测试和小范围集成测试结合，不写大而慢的 E2E
- 每批测试应能独立提交
- 先补红灯，再做最小实现修复

## 预期结果

- 框架主线对恢复、边界和并发的核心行为有更明确的回归保护
- 后续再重构 `session / dispatcher / bootstrap` 时，有更稳的护栏
