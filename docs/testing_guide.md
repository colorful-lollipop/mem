# MemRPC 与 VPS 测试指南

本文档以 `memrpc`、`virus_executor_service` 和相关测试为主线，重点回答四个问题：

1. 这个仓库有哪些测试层次。
2. 每类测试在验证哪部分 C++ 实现。
3. 平时改代码应该跑哪些命令。
4. 出现故障时，应该先看哪类测试和哪几个源码文件。

配套架构说明见 [architecture.md](architecture.md)。

## 1. 测试总原则

这个仓库的测试不是“只有 GoogleTest 单测”。它是一个分层体系：

- `memrpc/tests/`
  验证框架本身的协议、session、client/server、恢复和故障处理。
- `virus_executor_service/tests/unit/`
  验证 VPS 业务层、testkit、typed codec、policy、health 等薄封装行为。
- `virus_executor_service/tests/dt/`
  验证恢复、稳定性、性能基线等确定性测试场景。
- `virus_executor_service/tests/stress/`
  验证多线程、吞吐、配置和长时间运行行为。
- `virus_executor_service/tests/fuzz/`
  验证 codec 在异常输入下的稳健性。
- `integration` 标签测试
  验证 supervisor/服务进程/注册中心这类跨进程路径。

理解这些测试最重要的一点是：

- `memrpc` 测的是框架语义
- VPS 测的是“业务如何使用框架”

## 2. 先用哪个命令

仓库推荐的统一入口是：

```bash
tools/build_and_test.sh
```

常用变体：

```bash
tools/build_and_test.sh --clean
tools/build_and_test.sh --strict
tools/build_and_test.sh --asan
tools/build_and_test.sh --tsan
tools/build_and_test.sh --test-regex memrpc
tools/build_and_test.sh --test-regex virus_executor_service
tools/build_and_test.sh --label stress
tools/build_and_test.sh --repeat until-fail:100 --test-regex memrpc_engine_death_handler_test
```

对非平凡修改，尤其是并发、恢复、生命周期相关改动，推荐继续跑：

```bash
tools/push_gate.sh
tools/push_gate.sh --deep
```

## 3. 为什么很多测试必须提权运行

这个仓库的有效测试普遍依赖：

- `shm_open`
- `mmap`
- `eventfd`
- Unix domain socket
- `fork` / 子进程
- registry / service 启停

因此在 Codex 类沙箱里，完整 `ctest`、DT、stress、integration、sanitizer 路径往往需要提权。  
这不是测试写得“太重”，而是因为这些功能本来就在验证真实的共享内存和进程交互。

## 4. `memrpc` 测试在验证什么

`memrpc/tests/CMakeLists.txt` 把框架测试组织成一组可单独执行的目标。

### 4.1 基础结构与协议

代表测试：

- `memrpc_smoke_test`
- `memrpc_types_test`
- `memrpc_api_headers_test`
- `memrpc_framework_split_headers_test`
- `memrpc_protocol_layout_test`
- `memrpc_byte_codec_test`
- `memrpc_build_config_test`

主要覆盖：

- 头文件是否可独立包含
- `protocol.h` 中的布局常量是否正确
- 基础类型、字节读写和编解码是否稳定

这类测试改动触发点通常是：

- 改了 `protocol.h`
- 改了公共头文件依赖
- 改了 codec

对应重点源码：

- `memrpc/include/memrpc/core/protocol.h`
- `memrpc/include/memrpc/core/types.h`
- `memrpc/src/core/byte_reader.cpp`
- `memrpc/src/core/byte_writer.cpp`

### 4.2 session 与共享内存底层

代表测试：

- `memrpc_session_test`
- `memrpc_bootstrap_health_check_test`
- `memrpc_sa_bootstrap_stub_test`

主要覆盖：

- `Session::Attach()` 的协议校验和共享内存映射
- bootstrap channel 的打开、关闭和健康检查
- 开发/桩环境下 session 资源建立是否正确

对应重点源码：

- `memrpc/src/core/session.cpp`
- `memrpc/include/memrpc/core/bootstrap.h`
- `memrpc/src/bootstrap/dev_bootstrap.cpp`
- `memrpc/src/bootstrap/sa_bootstrap.cpp`

### 4.3 client/server 核心调用链

代表测试：

- `memrpc_rpc_client_api_test`
- `memrpc_rpc_server_api_test`
- `memrpc_rpc_server_executor_test`
- `memrpc_typed_future_test`
- `memrpc_rpc_payload_limits_test`

主要覆盖：

- 请求如何被提交、消费和回写
- typed future 与同步等待语义
- 高低优先级 worker 和 executor 背压
- payload 超限时的明确失败行为

对应重点源码：

- `memrpc/src/client/rpc_client.cpp`
- `memrpc/src/server/rpc_server.cpp`

### 4.4 恢复、watchdog 与异常路径

代表测试：

- `memrpc_rpc_client_timeout_watchdog_test`
- `memrpc_engine_death_handler_test`
- `memrpc_rpc_client_external_recovery_test`
- `memrpc_rpc_client_recovery_policy_test`
- `memrpc_rpc_client_shutdown_race_test`
- `memrpc_rpc_eventfd_fault_injection_test`

这组测试是理解框架的关键，因为它们对应 `RpcClient` 最复杂的部分。

重点验证：

- `execTimeoutMs` 到期后 waiter 是否及时解除阻塞
- 迟到响应是否被丢弃
- engine death 回调是否正确触发恢复决策
- external health signal 是否会驱动恢复
- `Shutdown()` 与 pending/restart 竞争时是否死锁
- eventfd 异常和注入故障是否正确传播

建议首先读两个代表文件：

- [`memrpc/tests/rpc_client_timeout_watchdog_test.cpp`](/root/code/demo/mem/memrpc/tests/rpc_client_timeout_watchdog_test.cpp)
- [`memrpc/tests/engine_death_handler_test.cpp`](/root/code/demo/mem/memrpc/tests/engine_death_handler_test.cpp)

这两个测试基本把 `RpcClient` 的超时、恢复、关闭语义都照亮了。

### 4.5 DT 稳定性与性能

代表测试：

- `memrpc_dt_stability_test`
- `memrpc_dt_perf_test`

这类测试只在开启 DT 路径时编译，目的是：

- 长时间重复验证核心行为不漂移
- 对关键性能指标做受控比较

## 5. VPS 测试在验证什么

VPS 测试可以分为五层来看。

### 5.1 业务单元测试

代表测试：

- `virus_executor_service_session_service_test`
- `virus_executor_service_heartbeat_test`
- `virus_executor_service_codec_test`
- `virus_executor_service_health_test`
- `virus_executor_service_policy_test`
- `virus_executor_service_recovery_reason_test`
- `virus_executor_service_health_subscription_test`
- `virus_executor_service_sample_rules_test`

主要覆盖：

- `VirusExecutorService` 控制面行为
- `VesEngineService` 的业务规则
- heartbeat 和 health snapshot 语义
- typed codec 和 policy 判定

对应重点源码：

- `virus_executor_service/src/service/virus_executor_service.cpp`
- `virus_executor_service/src/service/ves_engine_service.cpp`
- `virus_executor_service/src/ves/ves_sample_rules.cpp`

### 5.2 testkit 单元与轻量并发测试

代表测试：

- `virus_executor_service_testkit_headers_test`
- `virus_executor_service_testkit_codec_test`
- `virus_executor_service_testkit_service_test`
- `virus_executor_service_testkit_client_test`
- `virus_executor_service_testkit_dfx_test`
- `virus_executor_service_testkit_backpressure_test`
- `virus_executor_service_testkit_async_pipeline_test`
- `virus_executor_service_testkit_baseline_test`
- `virus_executor_service_testkit_latency_test`

这组测试的重要性非常高，因为它用最简单的 `Echo` / `Add` / `Sleep` handler 验证框架行为。

其中最值得读的是：

- [`virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp`](/root/code/demo/mem/virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp)

这个测试展示了：

- 同步调用吞吐
- 异步 pipeline batch 大小时的吞吐变化
- `RpcServer` worker 配置与 `TypedFuture` 的组合效果

如果你改的是：

- `RpcClient`
- `RpcServer`
- testkit client/service
- typed future

这组测试应该优先跑。

### 5.3 DT 与恢复测试

代表测试：

- `virus_executor_service_testkit_dt_stability_test`
- `virus_executor_service_testkit_dt_perf_test`
- `virus_executor_service_crash_recovery_test`

最值得重点阅读的是：

- [`virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`](/root/code/demo/mem/virus_executor_service/tests/dt/ves_crash_recovery_test.cpp)

它验证的是一条完整的真实路径：

- registry 启动
- 引擎进程拉起
- `VesClient` 建连
- 杀死引擎
- 观察恢复事件
- 等待重新加载并再次成功 `ScanFile`

它对应的实现链路覆盖很广：

- `VesClient`
- `VesBootstrapChannel`
- `RpcClient` 恢复状态机
- `VirusExecutorService`
- registry/transport

如果你改的是恢复、health、engine death、session reopen，这类 DT 测试是必须的。

### 5.4 stress 与 perf

代表测试：

- `virus_executor_service_testkit_throughput_test`
- `virus_executor_service_testkit_stress_config_test`
- `virus_executor_service_testkit_stress_smoke`
- `virus_executor_service_stress_test`

最有代表性的源码：

- [`virus_executor_service/tests/stress/testkit_throughput_test.cpp`](/root/code/demo/mem/virus_executor_service/tests/stress/testkit_throughput_test.cpp)

这个测试会：

- 拉起 `DevBootstrapChannel`
- 配置 `RpcServer` worker 线程数
- 用 `TestkitClient` 多线程循环执行 `Echo` / `Add` / `Sleep`
- 计算 ops/sec
- 和 baseline 文件比较

这类测试主要回答两个问题：

- 改动有没有明显拉低吞吐
- 多线程压力下有没有更快暴露队列/竞争问题

### 5.5 integration 与 supervisor

代表测试：

- `virus_executor_service_supervisor_integration_test`

它直接跑 supervisor 可执行程序，目的是验证：

- supervisor 进程生命周期
- transport 与服务装配
- 工作目录和运行时环境是否正确

如果你改的是 `src/app/` 或 transport/registry 相关代码，这类测试不能省。

### 5.6 fuzz

代表测试：

- `virus_executor_service_codec_fuzz_smoke`
- `virus_executor_service_testkit_codec_fuzz_smoke`

它们主要针对 codec 稳健性，适合在改动：

- `ves_codec`
- `testkit_codec`
- 协议结构体

时补跑。

## 6. 常见改动应该跑什么

### 6.1 只改 `protocol.h`、codec、typed 结构体

最少建议：

```bash
tools/build_and_test.sh --test-regex "memrpc_(protocol_layout|byte_codec|types|api_headers)|virus_executor_service_(codec|testkit_codec)"
```

如果改动影响 payload 大小、结构体布局或测试基线，再加：

```bash
tools/build_and_test.sh --test-regex "dt_perf|throughput"
```

### 6.2 改 `Session`、共享内存 attach、eventfd

最少建议：

```bash
tools/build_and_test.sh --test-regex "memrpc_(session|bootstrap_health_check|rpc_eventfd_fault_injection|rpc_server_api|rpc_client_api)"
```

更稳妥：

```bash
tools/push_gate.sh --deep
```

### 6.3 改 `RpcClient` 恢复、超时、idle close、shutdown

最少建议：

```bash
tools/build_and_test.sh --test-regex "memrpc_(rpc_client_timeout_watchdog|engine_death_handler|rpc_client_external_recovery|rpc_client_recovery_policy|rpc_client_shutdown_race)"
```

再补：

```bash
tools/build_and_test.sh --test-regex "virus_executor_service_crash_recovery"
tools/push_gate.sh --deep
```

### 6.4 改 `RpcServer` 调度、worker、completion queue

最少建议：

```bash
tools/build_and_test.sh --test-regex "memrpc_(rpc_server_api|rpc_server_executor)|virus_executor_service_testkit_(async_pipeline|backpressure|throughput)"
```

### 6.5 改 `VesClient`、`VirusExecutorService`、控制面旁路

最少建议：

```bash
tools/build_and_test.sh --test-regex "virus_executor_service_(session_service|heartbeat|health|policy|crash_recovery)"
```

如果改了恢复或 control reload，再加：

```bash
tools/build_and_test.sh --label dt
```

## 7. 代表性测试怎么读

### 7.1 `rpc_client_timeout_watchdog_test.cpp`

这个文件非常适合理解 `RpcClient` 的 timeout 语义：

- 慢 handler 是否触发 `ExecTimeout`
- waiter 是否会先返回
- 迟到 reply 是否被丢弃
- 队列阻塞是否触发 `QueueTimeout`

读这个文件时，建议同时打开：

- `memrpc/src/client/rpc_client.cpp`
- `memrpc/src/server/rpc_server.cpp`

因为测试场景本质是在用可控 handler 睡眠时间，驱动 client/server 两端状态变化。

### 7.2 `engine_death_handler_test.cpp`

这个文件适合理解恢复策略接口本身：

- 无 handler 时的兼容行为
- `onEngineDeath` 返回 Ignore/Restart 的语义
- `Shutdown()` 在 restart 请求中的收尾行为

它不追求完整进程级恢复，而是先把接口语义和状态稳定住。

### 7.3 `testkit_async_pipeline_test.cpp`

这个文件适合理解：

- 同步 facade 和异步 future 的性能关系
- batch 大小与吞吐的关系
- worker 配置对 pipeline 的影响

它是“轻量性能测试”和“并发行为测试”的交叉点。

### 7.4 `ves_crash_recovery_test.cpp`

这个文件适合理解：

- registry 如何拉起服务
- 引擎挂掉后 `VesClient` 如何恢复
- 恢复事件如何被观测
- 重新扫描如何证明恢复完成

如果你要排查“VesClient 恢复没生效”，这个文件通常比只看单测更有价值。

### 7.5 `testkit_throughput_test.cpp`

这个文件适合理解：

- 多线程吞吐如何被采样
- baseline 如何参与判断
- `Echo` / `Add` / `Sleep` 三种负载各自测什么

## 8. 测试失败时如何定位

### 8.1 `QueueFull`、背压、吞吐下降

先看：

- `memrpc/src/server/rpc_server.cpp`
- `memrpc/src/core/session.cpp`
- `virus_executor_service/tests/unit/testkit/testkit_backpressure_test.cpp`
- `virus_executor_service/tests/stress/testkit_throughput_test.cpp`

通常关注：

- ring 容量
- completion queue 容量
- response credit 等待
- worker 线程数是否不匹配

### 8.2 `ExecTimeout`、恢复、重连失败

先看：

- `memrpc/src/client/rpc_client.cpp`
- `virus_executor_service/src/client/ves_client.cpp`
- `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- `memrpc/tests/engine_death_handler_test.cpp`
- `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`

通常关注：

- timeout 的起点是不是理解错了
- 迟到 reply 是否还在污染后续调用
- recovery policy 是否返回了期望 action
- bootstrap health 和 sessionId 是否匹配

### 8.3 payload 过大、codec 失败

先看：

- `memrpc/include/memrpc/core/protocol.h`
- `virus_executor_service/src/client/ves_client.cpp`
- `memrpc/tests/rpc_payload_limits_test.cpp`
- `virus_executor_service/tests/unit/ves/ves_codec_test.cpp`
- `virus_executor_service/tests/unit/testkit/testkit_codec_test.cpp`

### 8.4 进程级 crash、DT 不稳定

先看：

- `virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`
- `virus_executor_service/src/service/virus_executor_service.cpp`
- `virus_executor_service/src/service/ves_session_service.cpp`
- transport/registry 相关实现

然后补跑：

```bash
tools/build_and_test.sh --label dt
tools/push_gate.sh --deep
```

## 9. 一个务实的日常验证矩阵

如果只是普通功能改动，推荐：

```bash
tools/build_and_test.sh --strict --test-regex "memrpc|virus_executor_service"
```

如果是共享内存、并发、恢复、生命周期改动，推荐：

```bash
tools/build_and_test.sh --asan
tools/build_and_test.sh --tsan
tools/push_gate.sh --deep
```

如果是性能相关改动，推荐：

```bash
tools/build_and_test.sh --test-regex "throughput|dt_perf|async_pipeline"
```

## 10. 结论

这个仓库的测试设计和架构设计是同一套思路：

- `memrpc` 负责把共享内存 RPC 的语义收紧、收清楚
- VPS 负责把业务 handler 和控制面接到框架上
- testkit 负责把复杂问题缩成最小可复现场景
- DT/stress/integration 负责证明真实进程和真实恢复路径能工作

如果你在读 C++ 代码时始终把“这段代码属于哪一层、对应哪类测试”挂在脑子里，定位问题会快很多。
