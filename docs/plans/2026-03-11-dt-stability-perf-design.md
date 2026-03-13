# DT 稳定性与性能回归测试设计

## 背景
当前默认 `ctest` 覆盖了 memrpc 与 minirpc 的功能性回归，但对短时稳定性与性能回归的约束不足。我们希望在不显著拉长 DT 的前提下，增加可重复、短时、可配置的稳定性与性能回归测试。

## 目标
- 在默认 `ctest` 中引入短时稳定性测试，覆盖并发、随机 payload 与队列混合。
- 引入性能回归测试，采用“相对基线 + 绝对底线”的双重约束。
- 默认测试每个用例 2–5 秒，长时模式通过环境开关启用。
- 基线按机器生成，写入 `build/` 目录并自动更新。

## 非目标
- 长时间压力测试、ASan/TSan/UBSan 长跑与 fuzz 仍保持独立（不并入 DT）。
- 不做跨机/跨环境稳定基线对比（基线不提交仓库）。

## 约束与范围
- 范围：默认 `ctest`（DT）覆盖 `memrpc` 与 `minirpc`。
- 运行时间：每个测试 2–5 秒，CI 默认仍为短时。
- 扩展模式：通过环境变量提升时长与负载。

## 总体方案
新增 4 个测试文件，并打上 `ctest` label：
- `tests/memrpc/dt_stability_test.cpp`（label: `dt_stability`）
- `tests/memrpc/dt_perf_test.cpp`（label: `dt_perf`）
- `tests/apps/minirpc/minirpc_dt_stability_test.cpp`（label: `dt_stability`）
- `tests/apps/minirpc/minirpc_dt_perf_test.cpp`（label: `dt_perf`）

## 负载模型（稳定性）
- 请求类型：
  - memrpc：`InvokeAsync` 为主，少量 `InvokeSync` 混入。
  - minirpc：`Echo/Add/Sleep` 按权重随机（如 70/25/5）。
- payload 大小：从集合中随机取值（如 `0/128/512/2K/4K/接近上限`）。
- 队列：高/普通队列随机分流。
- 并发：默认 `min(4, hw_threads)`，可配置。

## 稳定性测试设计
### memrpc_dt_stability
- 启动 server/client，注册基础 handler。
- 多线程短时循环发起请求，统计成功/失败计数。
- 进度监测：若超过 `MEMRPC_DT_progressTimeoutMs` 无成功请求，判定为停滞失败。
- 断言：
  - 全程 `StatusCode::Ok`。
  - 无 `ProtocolMismatch/QueueTimeout/ExecTimeout`。
  - 成功请求数 > 0。

### minirpc_dt_stability
- 启动 minirpc server/client。
- 多线程短时循环随机调用 `Echo/Add/Sleep`。
- 断言与进度监测规则同 memrpc。

## 性能回归设计
### 指标
- 吞吐：ops/s
- 延迟：p99（微秒）

### 基线
- 位置：`$BUILD_DIR/perf_baselines/`（默认 `build/perf_baselines/`）
- 形式：`key=value`（与现有 throughput baseline 兼容）
- 自动更新：若当前值优于基线则写回；若低于基线阈值则失败

### memrpc_dt_perf
- 用例建议：
  - `echo_0B`、`echo_4KB`
- 基线 key 示例：
  - `memrpc.echo_0B.threads=4.ops_per_sec`
  - `memrpc.echo_0B.threads=4.p99_us`
- 判定：
  - `ops/s < baseline * 0.9` -> FAIL
  - `p99_us > baseline * 1.1` -> FAIL
  - 低于绝对底线或高于绝对上限 -> FAIL

### minirpc_dt_perf
- 用例建议：
  - `echo_0B`、`echo_4KB`、`add`
- 基线 key 规则与 memrpc 一致，前缀改为 `minirpc.*`。

## 绝对底线（默认值，可调）
- `MEMRPC_DT_MIN_OPS`：默认 `50 ops/s`
- `MEMRPC_DT_MAX_P99_US`：默认 `20000 us`

> 说明：绝对底线设置为保守值，主要防止“完全失速”类回归；精细回归由基线兜底。

## 环境变量与开关
- `MEMRPC_DT_durationMs`：默认 3000
- `MEMRPC_DT_WARMUP_MS`：默认 200
- `MEMRPC_DT_THREADS`：默认 `min(4, hw_threads)`
- `MEMRPC_DT_progressTimeoutMs`：默认 200
- `MEMRPC_DT_MIN_OPS`：默认 50
- `MEMRPC_DT_MAX_P99_US`：默认 20000
- `MEMRPC_DT_EXTENDED=1`：延长时长（如 10000ms）并扩大样本

## CTest Label 策略
- `dt_stability`：短时稳定性
- `dt_perf`：性能回归

通过 `ctest -L dt_perf` 可单独运行；长时模式通过环境变量启用。

## 风险与缓解
- 性能波动导致误报：采用保守绝对阈值 + 相对基线双重约束，并保持短时样本量。
- 基线漂移掩盖回退：只允许“性能提升时更新”，回退立即失败。

## 预期结果
- DT 在不显著延长时长的情况下，补齐短时稳定性与性能回归覆盖。
- 通过标签与环境开关，实现默认短时、可选长时的测试策略。
