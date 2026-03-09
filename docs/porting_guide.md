# 迁移说明

## 目标

这套框架的目标是尽量保持旧业务层调用方式不变，把原来进程内的同步调用迁移成跨进程共享内存调用。

旧代码通常类似：

```cpp
ScanResult result = engine->Scan(request);
```

迁移后通常变成：

```cpp
memrpc::EngineClient client(bootstrap);
client.Init();

memrpc::ScanResult result;
client.Scan(request, &result);
```

## 建议迁移方式

- 业务侧尽量保留原有请求/结果结构
- 在兼容层把旧结构转换成 `memrpc::ScanRequest`
- 引擎侧把原来的扫描实现挂到 `IScanHandler`
- 进程拉起、重启、系统能力接入都放在 bootstrap/上层控制，不要混进业务扫描逻辑

## 当前恢复行为

当前实现中：

- 如果 SA/bootstap 报告子进程死亡，旧会话上的等待请求会立即失败
- 后续新的 `Scan()` 会自动尝试重建会话
- 可能已经被旧引擎看到的请求不会自动重放

所以业务上仍然应该保留“失败后是否重扫”的最终决策权。

## Linux 与鸿蒙的差异

- Linux 开发态可以直接使用 demo 或 fake SA
- HarmonyOS 正式环境中，进程应由 `init` 拉起
- 客户端通过 `GetSystemAbility` / `LoadSystemAbility` 获得服务和句柄

也就是说，迁移到鸿蒙时重点是补平台适配层，而不是重写通信框架。
