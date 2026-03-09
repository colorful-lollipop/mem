# Demo 使用说明

## 编译

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/demo/memrpc_minirpc_demo
```

当前主线 demo 只保留 `MiniRpc`，用于验证最小跨进程 RPC 主路径。

预期输出：

- `echo: hello`
- `add: 15`

## 当前验证点

这个 demo 在 Linux 开发环境下使用 `fork()` 启动子进程，主要验证：

- 共享内存 + `eventfd` 的基础通信
- 高优请求队列 / 普通请求队列
- 单响应队列 + 单 `resp_eventfd`
- `Reply/Event` 共用响应队列的协议基础
- `RpcClient` / `RpcServer` 公共接口
- `MiniRpcAsyncClient` / `MiniRpcClient` 应用层写法

当前 demo 不再演示独立 notify 通道，也不再承担 VPS 复杂业务兼容验证。

## 说明

这不是鸿蒙正式部署方式。迁移到 HarmonyOS 时，应保留通信核心不变，只把 bootstrap 替换成基于 `GetSystemAbility` / `LoadSystemAbility` 的实现，并由 `init` 管理子进程生命周期。
