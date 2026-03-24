# 当前文档入口

如果只看一页，先看这里；再按需要展开到下面几份：

- [plan.md](/root/mem/plan.md)：当前设计结论、边界和非目标
- [architecture.md](/root/mem/docs/architecture.md)：协议、并发、恢复模型
- [porting_guide.md](/root/mem/docs/porting_guide.md)：新业务如何接入
- [demo_guide.md](/root/mem/docs/demo_guide.md)：如何构建、运行和验证主线示例
- [stress_fuzz_guide.md](/root/mem/docs/stress_fuzz_guide.md)：压力、模糊测试、Sanitizer 入口

## 一句话版本

当前主线已经收敛为：

- `memrpc` 只做固定 entry 的小包共享内存 RPC
- 对外业务接口默认同步
- 生命周期与恢复只由 `RpcClient` 统一管理
- VES 侧最多只保留一条业务降级旁路：超大请求直接走同步 `AnyCall`
- 超大响应和超大事件直接失败
- `docs/plans/` 已不再保留历史设计归档

## 当前关键事实

- request / response ring entry 固定 `512B`
- request inline 上限 `480B`
- response inline 上限 `472B`
- 不再有 slot、slot pool、slot state
- VES 控制面已经提供 `AnyCall`
- `VesClient` 只是类型化薄封装，不再自带本地恢复循环
- `EngineDeath` 只负责让旧 session / control 失效并上报事件；control 刷新发生在显式取用点
- 恢复观测以统一恢复快照和事件报告为准，不再给 facade 额外扩测试语义口

## 建议阅读顺序

- 先看 [architecture.md](/root/mem/docs/architecture.md)，理解状态机、恢复窗口和 `Disconnected`
- 再看 [recovery_ownership.md](/root/mem/docs/recovery_ownership.md)，理解 `RpcClient` / `VesClient` / 业务降级的职责边界
- 最后按需要看 [porting_guide.md](/root/mem/docs/porting_guide.md) 和 [demo_guide.md](/root/mem/docs/demo_guide.md)

## 关于 `docs/plans/`

`docs/plans/` 现在只保留一个入口说明：[docs/plans/guide.md](/root/mem/docs/plans/guide.md)。

历史方案如果需要追溯，直接看 `git log` / `git show`，不要再把它们当成当前规范来源。
