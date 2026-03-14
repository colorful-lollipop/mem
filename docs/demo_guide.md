# Demo 使用说明

## 构建

仓库主线使用根构建目录：

```bash
tools/build_and_test.sh --build-only
```

如果只想手工编译主线可执行体：

```bash
cmake -S . -B build_ninja -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
cmake --build build_ninja --parallel
```

## 运行

启动 supervisor：

```bash
./build_ninja/virus_executor_service/virus_executor_service_supervisor
```

推荐 smoke：

```bash
ctest --test-dir build_ninja --output-on-failure -R virus_executor_service_supervisor_integration_test
```

## 当前 demo 验证什么

当前主线 demo 是 `virus_executor_service`。它验证的是“固定 entry 的小包共享内存 RPC + 同步控制面兜底”，不是旧的 `ring + slot` 方案。

主要验证点：

- 高优请求队列 / 普通请求队列 / 响应队列
- 固定 `512B` request / response ring entry
- 请求和响应正文直接内嵌在 ring entry 中
- `Reply/Event` 共用响应队列
- `RpcClient` / `RpcServer` 公共接口
- `VesClient` 对外同步 typed API
- 大请求通过 `AnyCall` 控制面兜底
- 大响应或大事件返回 `PayloadTooLarge`
- `VesControlProxy` 保留 heartbeat 与健康快照协议，`RpcClient` watchdog 统一调度恢复

当前不是主线验证点的内容：

- request / response slot 生命周期
- 大响应 heartbeat 回拉
- `memrpc` 执行后自动 fallback 到 `AnyCall`

## 手动运行

```bash
./build_ninja/virus_executor_service/virus_executor_service_client
./build_ninja/virus_executor_service/virus_executor_service_stress_client --threads 2 --iterations 100
./build_ninja/virus_executor_service/virus_executor_service_testkit_stress_runner
```

## HarmonyOS 迁移说明

这个 demo 不是 HarmonyOS 正式部署方式。迁移到正式环境时，原则仍然是：

- 保留 `memrpc` 通信核心
- 保留 VES 控制面协议
- 只替换 bootstrap / SA 接入方式
- 由 `init` 管理子进程生命周期
