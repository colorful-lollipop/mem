# 当前文档入口

如果你现在要理解本项目，优先看这三份：

- [architecture.md](/root/code/demo/mem/docs/architecture.md)：以 C++ 实现为主线，系统讲清 `memrpc`、VPS、testkit、调用链和关键类职责
- [testing_guide.md](/root/code/demo/mem/docs/testing_guide.md)：测试体系、代表性测试、常用命令和“改哪里该跑什么”
- [memrpc_vesclient_code_walkthrough.md](/root/code/demo/mem/docs/memrpc_vesclient_code_walkthrough.md)：按函数和源码入口导读 `RpcClient`、`VesClient` 与 typed 调用链
- [recovery_ownership.md](/root/code/demo/mem/docs/recovery_ownership.md)：恢复职责边界、`RpcClient` / `VesClient` 分工

按需继续看：

- [porting_guide.md](/root/code/demo/mem/docs/porting_guide.md)：新业务如何接入
- [demo_guide.md](/root/code/demo/mem/docs/demo_guide.md)：如何构建、运行和验证示例
- [stress_fuzz_guide.md](/root/code/demo/mem/docs/stress_fuzz_guide.md)：压力、模糊测试、Sanitizer 入口
- [sa_integration.md](/root/code/demo/mem/docs/sa_integration.md)：系统能力接入相关说明

## 一句话版本

当前主线已经收敛为：

- `memrpc` 负责固定 entry 的小包共享内存 RPC
- `RpcClient` 统一持有 session 生命周期、超时和恢复状态机
- `RpcServer` 负责 request ring 消费、worker 调度和 response ring 回写
- VPS 通过 `EngineSessionService` 把业务 handler 同时接到 MemRPC 主路径和 `AnyCall` 旁路
- `VesClient` 是 typed 薄封装，主要负责 codec、恢复策略装配和主路径/旁路选择
- testkit 是理解框架与验证并发/恢复路径的核心辅助层

## 推荐阅读顺序

1. [architecture.md](/root/code/demo/mem/docs/architecture.md)
2. [testing_guide.md](/root/code/demo/mem/docs/testing_guide.md)
3. [recovery_ownership.md](/root/code/demo/mem/docs/recovery_ownership.md)
4. [porting_guide.md](/root/code/demo/mem/docs/porting_guide.md)

## 关于 `docs/plans/`

`docs/plans/` 主要用于过程性实现计划，不应替代当前架构规范。  
如果要追溯历史方案，请直接查 `git log` / `git show`。
