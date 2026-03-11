# 框架与 minirpc 稳定性/并发压力与 Fuzz 测试设计

## 目标

- 系统性验证 `memrpc` 框架与 `minirpc` 应用在高并发与长时间运行下的稳定性。
- 发现并定位内存越界、泄漏、竞态、共享内存通信异常等问题。
- 安全测试先聚焦收益最高的输入面：`minirpc` 编解码与 payload 解析路径的 fuzz。

## 范围

- 环境：单机 Linux + root。
- 稳定性：Release 构建，连续 2 小时随机负载长跑。
- 并发压力：多线程并发请求、随机 payload、突发与平稳交替。
- 安全：`minirpc` 编解码与 payload 解析路径 fuzz。

## 非目标

- 不做共享内存协议/环形队列的深度 fuzz（除非后续追加）。
- 不覆盖业务层/legacy 兼容层渗透测试。
- 不做对外发布的 benchmark 报告。

## 总体方案（分阶段混合）

- `ASan + UBSan + LSan`：短时高强度压力，用于快速发现内存与 UB。
- `TSan`：缩短版并发压力，专注竞态发现。
- `Release`：2 小时随机负载，验证真实稳定性。
- `libFuzzer`：独立 fuzz 编解码与解析路径。

## 构建与隔离策略（与日常 DT 分离）

- 独立构建目录与产物：`build_stress/`、`build_tsan/`、`build_asan/`、`build_fuzz/`。
- 测试分组使用独立 `ctest` label（如 `stress`/`fuzz`），不进入默认 `ctest`。
- 提供独立脚本/命令入口，只有显式调用才执行。

## 负载模型（minirpc）

- 请求 mix：`Echo/Add/Sleep` 按权重随机（默认 70/20/10，可配置）。
- payload 大小：`0/16/128/512/1K/2K/4K/接近上限` 中随机。
- 优先级：高/普通队列随机分流。
- 节奏：随机突发 + 平稳交替（阶段式波动）。
- 并发线程：按 CPU 核数上下浮动，可配置最大值。

## 执行矩阵

- `ASan + UBSan + LSan`：10-30 分钟高强度压力。
- `TSan`：10-20 分钟并发压力，降低负载以减少噪声。
- `Release`：2 小时随机负载长跑。

## Fuzz 方案

- 目标：`minirpc` 编解码与 payload 解析路径（`DecodeMessage`、typed handler）。
- 工具：`libFuzzer` + `-fsanitize=fuzzer,address,undefined`。
- 独立构建开关：`MEMRPC_ENABLE_FUZZ=ON`。
- 1-2 个 fuzz 目标覆盖 `Echo/Add/Sleep` 编解码。
- 维护最小种子 corpus（有效样本 + 边界样本）。

## 观测与通过标准

- `ASan/UBSan/LSan/TSan` 报告必须为 0。
- Release 长跑 2 小时内无崩溃、死锁或无进展（例如 30 秒无成功 RPC）。
- RPC 失败率为 0，仅允许 `StatusCode::Ok`。
- 共享内存/协议异常为 0（如 `ProtocolMismatch/QueueTimeout/ExecTimeout`）。
- 内存稳定：热身 10 分钟后 RSS 增长 < 5% 或 < 50MB（取较大值）。

## 产出

- 可重复执行的压力/稳定性脚本与配置。
- 可复用的 fuzz 目标与最小 corpus。
- 统一的日志与结果汇总输出。

