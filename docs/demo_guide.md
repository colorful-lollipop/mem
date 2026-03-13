# Demo 使用说明

## 编译

```bash
cmake -S . -B build
cmake --build build --target vpsdemo_supervisor vpsdemo_client vpsdemo_stress_client vpsdemo_testkit_stress_runner
```

## 运行

```bash
./build/vpsdemo/vpsdemo_supervisor
```

推荐的自动化 smoke 方式：

```bash
ctest --test-dir build --output-on-failure -R vpsdemo_supervisor_integration_test
```

## 当前验证点

当前主线 demo 是 `vpsdemo`。它在 Linux 开发环境下使用 supervisor 拉起 engine SA，并在同一条 `memrpc` 通道上同时承载：

- `ves` 业务调用
- `testkit` 的 `Echo/Add/Sleep` 与故障注入 RPC

主要验证点：

- 共享内存 + `eventfd` 的基础通信
- 高优请求队列 / 普通请求队列
- 单响应队列 + 单 `resp_eventfd`
- 请求 slot 只承载请求内容
- 响应通过 response ring + response slot 返回
- `Reply/Event` 共用响应队列的协议基础
- `RpcClient` / `RpcServer` 公共接口
- `VesClient` 与 `TestkitClient` 两类应用 facade 共用同一引擎服务端
- 业务集成测试、testkit perf/stress/DT/fuzz 测试共用一套主线可执行体

手动压测可直接运行：

```bash
./build/vpsdemo/vpsdemo_stress_client --threads 2 --iterations 100 --seed 42 --no-crash
./build/vpsdemo/vpsdemo_testkit_stress_runner
```

## 说明

这不是鸿蒙正式部署方式。迁移到 HarmonyOS 时，应保留通信核心不变，只把 bootstrap 替换成基于 `GetSystemAbility` / `LoadSystemAbility` 的实现，并由 `init` 管理子进程生命周期。
