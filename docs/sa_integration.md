# HarmonyOS SA 接入说明

## 分层原则

`memrpc` 的共享内存通信核心不依赖鸿蒙 SA 头文件。  
SA 相关逻辑只应出现在 bootstrap 层。

这样做的目的有两个：

- Linux 下可以独立开发、调试、跑 demo
- 迁移到 HarmonyOS 时只替换平台适配部分

## 当前仓库里的 SA 现状

当前仓库里的 `memrpc::SaBootstrapChannel` 不是正式鸿蒙实现，而是一个开发态 fake SA 适配层。

它的作用是：

- 把接口形状提前固定下来
- 在 Linux 下复用现有 POSIX 共享内存和 `eventfd` 通道
- 支持死亡回调、session 切换和恢复测试

## 鸿蒙正式环境下建议承担的职责

正式 SA 适配层建议负责：

- 发现或启动引擎服务
- 交换共享内存 fd
- 交换请求/响应 `eventfd`
- 把子进程死亡事件回调给 `EngineClient`
- 在重启后让 `Connect()` 返回最新 `BootstrapHandles`

## 建议接入流程

1. 客户端通过 `GetSystemAbility` 获取服务代理
2. 需要拉起或重建引擎时调用 `LoadSystemAbility`
3. 通过 SA 交换 `BootstrapHandles`
4. `EngineClient` 使用这些句柄附着共享内存会话
5. 若引擎死亡，SA 回调通知客户端
6. 下一次 `Scan()` 自动重新 `StartEngine()` + `Connect()`

## 当前恢复语义

当前代码实现的是保守恢复模型：

- 死亡回调到来后，旧 `session` 立即失效
- 旧 `session` 上等待中的请求立即失败
- 下一次 `Scan()` 懒恢复到新 `session`
- 已经发布到旧 ring 的请求不自动重放

这个边界比较适合先接入业务，再由上层决定是否做更激进的重试策略。
