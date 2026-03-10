# MiniRpc View Decode + Throughput Baseline Design

## 背景

框架层已经具备 view 能力（`PayloadView`、`ByteReader::Read*View`、move-aware request/reply），
但 `MiniRpc` 目前只有 `EchoRequest` 走 view decode。为了评估“应用层是否也应优化”，
需要让 `MiniRpc` 全量覆盖 view 路径，并在同一仓库内提供吞吐量对比测试。

## 目标

- 为 `MiniRpc` 所有 request/reply 增加 view 版 decode，保留 owning decode 作为兼容路径。
- 服务端默认使用 view decode，调用方仍可选择 owning decode。
- 新增 GTest 吞吐量测试，第一次运行生成基线，后续若下降超过 10% 视为失败。
- 基线策略“只上调不下调”：性能提升可自动更新基线，避免永久回退。

## 非目标

- 不修改 shared memory 协议或 `memrpc` 核心结构。
- 不引入第三方 benchmark 框架。
- 不做跨机器、跨环境的稳定基线保证。
- 不扩展 VPS 或其他业务层。

## 设计概览

### 1) 类型与编解码

新增 view 类型并配套 `ViewTraits` 解码：

- `EchoRequestView`（已有）
- 新增：
  - `EchoReplyView`
  - `AddRequestView` / `AddReplyView`
  - `SleepRequestView` / `SleepReplyView`

规则：

- `EncodeMessage` 保持 owning 输出，不新增 view encode。
- `DecodeMessage` 保留 owning 路径。
- `DecodeMessageView` 仅做 view decode，不改变原有行为。

### 2) 服务端默认 view decode

`MiniRpcService` 默认注册 view decode handler（`Echo/Add/Sleep` 全部使用 view）。
同时保留 owning decode 入口，便于测试对比。

推荐做法：在 `RegisterHandlers` 增加 `DecodeMode` 参数（`Owning` / `View`），
默认 `View`，测试可显式选择 `Owning`。

### 3) 吞吐量测试

新增 `tests/apps/minirpc/minirpc_throughput_test.cpp`：

- 目标：测 `Echo/Add/Sleep(delay=0)` 三种 RPC 的吞吐量（ops/sec）。
- 多并发：默认 `min(4, hardware_concurrency)`，可用环境变量覆盖。
- 运行时长：固定窗口（含 warmup）。
- 同时测 `view` 与 `owning` 两种 handler 模式。

#### 基线策略

- 基线文件默认：`build/perf_baselines/minirpc_throughput.baseline`
- 若基线不存在：生成并通过。
- 若基线存在：
  - 当前值 < 基线 * 0.9 => 失败
  - 当前值 > 基线 => 自动上调基线

### 4) 运行参数

通过环境变量控制测试稳定性：

- `MEMRPC_PERF_THREADS`：并发线程数
- `MEMRPC_PERF_DURATION_MS`：测量时长（不含 warmup）
- `MEMRPC_PERF_WARMUP_MS`：预热时长
- `MEMRPC_PERF_BASELINE_PATH`：基线文件路径

## 风险与约束

- 性能测试易受机器负载影响，需要基线“只上调”策略减少误报。
- view 类型不能跨线程或持久化持有，必须限定在 handler 调用栈内。
- 基线文件格式需足够简单，避免引入 JSON 解析等依赖。

## 预期结果

- `MiniRpc` 既支持 owning，也支持 view decode。
- 吞吐量测试能稳定输出基线，并在显著回退时报警。
- 保持框架简洁，复杂度只落在样板层和测试层。
