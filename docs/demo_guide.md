# Demo 使用说明

## 编译

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/demo/memrpc_demo_dual_process
```

预期会看到三类输出：

- 普通请求返回 `kClean`
- 高优请求返回 `kInfected`
- 慢请求返回 `kExecTimeout`

## 说明

这个 demo 运行在 Linux 开发环境下，使用 `fork()` 启动子进程，方便本地验证共享内存、`eventfd`、优先级队列和恢复逻辑。

这不是鸿蒙正式部署方式。迁移到 HarmonyOS 时，应保留通信核心不变，只把 bootstrap 替换成基于 `GetSystemAbility` / `LoadSystemAbility` 的实现，并由 `init` 管理引擎进程生命周期。
