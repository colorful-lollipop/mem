# Vpsdemo Temp Perf Harness Design

**Goal:** Collect local-only performance data (idle CPU baseline + stress p50/p99 latency + CPU) without modifying repository code paths.

**Scope**
- Create a temporary standalone C++ harness under `/tmp` that links against existing `vpsdemo`/`memrpc` libraries.
- Do not add or modify production code. No new tests are registered in CMake.
- Produce a simple text report: idle CPU, stress throughput, p50/p99 latency.

**Architecture**
- Harness spawns a local registry server and forks `vpsdemo_engine_sa` using Unix sockets under `/tmp`.
- The harness sets up `RegistryBackend`, obtains `SystemAbility`, and initializes `VpsClient` to create a session.
- Two phases:
  - **Idle phase:** no RPC calls, sample CPU usage for a fixed duration.
  - **Stress phase:** N threads issue `ScanFile` calls, record per-call latency, compute p50/p99.

**Measurements**
- CPU sampling uses `/proc/stat` + `/proc/<pid>/stat` deltas across an interval. Report engine, harness, and combined CPU%.
- Latency is recorded in microseconds for each RPC; report p50/p99 and ops/s.
- Crash injection is disabled for stable measurements.

**Workload**
- Use deterministic, non-sleep sample paths (e.g., `clean`/`virus`) to avoid artificial delays.
- Default threads and iterations are configurable via CLI args.

**Failure Handling**
- If `LoadSystemAbility` or `VpsClient::Init()` fails, exit with a clear error.
- On SIGINT/SIGTERM, stop registry, kill engine, and clean up sockets.

**Outputs**
- Print a concise summary block to stdout with durations, CPU%, throughput, and latency percentiles.

