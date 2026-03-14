# 当前文档入口

如果只看一页，先看这里；再按需要展开到下面几份：

- [plan.md](/root/mem/plan.md)：当前设计结论、边界和非目标
- [architecture.md](/root/mem/docs/architecture.md)：协议、并发、恢复模型
- [porting_guide.md](/root/mem/docs/porting_guide.md)：新业务如何接入
- [demo_guide.md](/root/mem/docs/demo_guide.md)：如何构建、运行和验证主线 demo
- [stress_fuzz_guide.md](/root/mem/docs/stress_fuzz_guide.md)：压力、fuzz、sanitizer 入口

## 一句话版本

当前主线已经收敛为：

- `memrpc` 只做固定 entry 的小包共享内存 RPC
- 对外业务接口默认同步
- 超大请求直接走同步 `AnyCall`
- 超大响应和超大事件直接失败
- `docs/plans/` 已不再保留历史设计归档

## 当前关键事实

- request / response ring entry 固定 `512B`
- request inline 上限 `480B`
- response inline 上限 `472B`
- 不再有 slot、slot pool、slot state
- VES 控制面已经提供 `AnyCall`

## 关于 `docs/plans/`

`docs/plans/` 现在只保留一个入口说明：[docs/plans/guide.md](/root/mem/docs/plans/guide.md)。

历史方案如果需要追溯，直接看 `git log` / `git show`，不要再把它们当成当前规范来源。
