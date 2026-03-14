# Stress & Fuzz Guide

## 推荐入口

优先使用仓库脚本，而不是单独在子目录手工配构建：

```bash
tools/build_and_test.sh --help
tools/ci_sweep.sh
```

常用命令：

```bash
tools/build_and_test.sh --test-regex virus_executor_service
tools/build_and_test.sh --label stress
tools/build_and_test.sh --asan
tools/build_and_test.sh --tsan
tools/build_and_test.sh --fuzz
tools/push_gate.sh
tools/push_gate.sh --deep
```

## 当前关注点

和当前主线设计最相关的压力点是：

- 固定 entry 小包主路径稳定性
- request/response ring 满载时的背压行为
- session 重建、heartbeat failure、engine death
- 大请求切到同步 `AnyCall`
- 大响应 / 大事件返回 `PayloadTooLarge`

## 手动 stress

```bash
ctest --test-dir build_ninja --output-on-failure -L stress
./build_ninja/virus_executor_service/virus_executor_service_testkit_stress_runner
```

如果要扩大随机覆盖，跨过 inline 阈值的 payload 大小也值得保留，用来同时覆盖：

- 小请求的 `memrpc` 主路径
- 大请求的 `AnyCall` 兜底路径

## 手动 fuzz

```bash
tools/build_and_test.sh --fuzz
ctest --test-dir build_ninja --output-on-failure -L fuzz
```

## 手动 sanitizer

```bash
tools/build_and_test.sh --asan
tools/build_and_test.sh --tsan
```
