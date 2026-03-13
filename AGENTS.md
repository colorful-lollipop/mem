# Repository Guidelines

## Project Structure & Module Organization

Current mainline work focuses on the shared-memory RPC framework and the VPS demo app.

- `memrpc/include/memrpc/`, `memrpc/src/`: framework code
- `vpsdemo/include/vpsdemo/`, `vpsdemo/src/{app,client,service,transport,testkit,ves}/`: mainline application code
- `memrpc/tests/`: focused framework unit and integration tests
- `vpsdemo/tests/`: app-level tests split by type: `unit/`, `integration/`, `stress/`, `dt/`, `fuzz/`
- `vpsdemo/perf_baselines/`: app-owned perf/DT baselines
- `docs/`: architecture, porting notes, and implementation plans

Keep business-specific compatibility layers out of the framework. Applications should adapt their own legacy APIs.

## Build, Test, and Development Commands

**本项目使用 Clang 工具链编译。**

- First choice: `tools/build_and_test.sh`
  This is the canonical zero-knowledge entrypoint for configure + build + test.
- `tools/build_and_test.sh`
  Configure, build, and run the full repo test suite with `clang + ninja` into `build_ninja/`.
- `tools/build_and_test.sh --clean`
  Recreate the default `build_ninja/` directory from scratch.
- `tools/build_and_test.sh --fuzz`
  Enable both memrpc and vpsdemo fuzz targets, then build and test them.
- `tools/build_and_test.sh --test-regex vpsdemo`
  Build everything, then only run tests matching `vpsdemo`.
- `tools/build_and_test.sh --label stress`
  Build everything, then only run `ctest -L stress`.
- `tools/build_and_test.sh --configure-only`
  Configure only.
- `tools/build_and_test.sh --build-only`
  Configure and build, but skip tests.
- `./build_ninja/vpsdemo/vpsdemo_supervisor`
  Run the VPS demo supervisor after building.
- `./build_ninja/vpsdemo/vpsdemo_client`
  Run the standalone VPS client after building.
- `./build_ninja/vpsdemo/vpsdemo_stress_client --threads 2 --iterations 100`
  Run the stress client directly.
- `./build_ninja/vpsdemo/vpsdemo_testkit_stress_runner`
  Run the testkit stress runner directly.
- Manual fallback: `cmake -S . -B build_ninja -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVPSDEMO_ENABLE_TESTS=ON`
- Manual build fallback: `cmake --build build_ninja --parallel`
- Manual test fallback: `ctest --test-dir build_ninja --output-on-failure --parallel`

### AI Quick Start

- If you are an AI agent with no repo context, start with `tools/build_and_test.sh --help`, then use `tools/build_and_test.sh`.
- Prefer the root build over ad-hoc per-module builds unless the task is isolated to `vpsdemo/`.
- Default build directory is `build_ninja/`. Do not reuse old `build/` directories configured with another generator.
- This repo's meaningful tests use shared memory, Unix-domain sockets, and child processes. In sandboxed agent environments, `ctest` may fail with `shm_open ... errno=13` or registry start failures unless you request elevated permissions.
- In Codex-style sandboxes, request escalation before running full or integration-heavy test commands. Typical cases: `tools/build_and_test.sh`, `ctest --test-dir build_ninja ...`, or any `vpsdemo_*integration*`, `stress`, `dt`, or shared-memory session tests.

Use CMake as the source of truth during active development.

## Coding Style & Naming Conventions

- Use C++17 and 4-space indentation.
- Prefer simple, explicit code over template-heavy or macro-heavy abstractions.
- Types, classes, functions, and methods use `UpperCamelCase`: `InitSession`, `PublishEvent`
- Variables, parameters, and members use `lowerCamelCase`: `requestId`, `eventType`
- Constants and macros use `ALL_CAPS_WITH_UNDERSCORES`: `DEFAULT_TIMEOUT_MS`
- Place new code under `OHOS::Security::VirusProtectionService`; framework code should live under `...::MemRpc`
- For framework headers, only use layered public include paths:
  - `memrpc/core/*`
  - `memrpc/client/*`
  - `memrpc/server/*`

New code must follow these rules. Older code may be cleaned up opportunistically when touched.

## Logging

Use `virus_protection_service_log.h` only. Prefer Harmony-style calls such as `HILOGI(...)`, `HILOGW(...)`, and `HILOGE(...)`. Add logs only on important state changes, failures, or recovery paths.

## Testing Guidelines

Tests use GoogleTest via CMake. Name new tests by feature, for example `response_queue_event_test.cpp` or `testkit_client_test.cpp`.

- Keep tests focused on core behavior
- Add framework tests for transport, session, queue, and recovery logic
- Add app tests only for thin integration paths

Avoid broad end-to-end additions unless they protect a real regression.

### vpsdemo Tests

The vpsdemo module includes both GoogleTest unit tests and executable integration tests:

| Test | Type | Command |
|------|------|---------|
| vpsdemo_session_service_test | unit | `ctest -R vpsdemo_session` |
| vpsdemo_heartbeat_test | unit | `ctest -R vpsdemo_heartbeat` |
| vpsdemo_codec_test | unit | `ctest -R vpsdemo_codec` |
| vpsdemo_testkit_headers_test | unit | `ctest -R vpsdemo_testkit_headers` |
| vpsdemo_testkit_codec_test | unit | `ctest -R vpsdemo_testkit_codec` |
| vpsdemo_testkit_service_test | unit | `ctest -R vpsdemo_testkit_service` |
| vpsdemo_testkit_client_test | unit | `ctest -R vpsdemo_testkit_client` |
| vpsdemo_testkit_dfx_test | unit | `ctest -R vpsdemo_testkit_dfx` |
| vpsdemo_testkit_backpressure_test | unit | `ctest -R vpsdemo_testkit_backpressure` |
| vpsdemo_testkit_async_pipeline_test | unit | `ctest -R vpsdemo_testkit_async_pipeline` |
| vpsdemo_testkit_baseline_test | unit | `ctest -R vpsdemo_testkit_baseline` |
| vpsdemo_testkit_latency_test | unit | `ctest -R vpsdemo_testkit_latency` |
| vpsdemo_testkit_throughput_test | perf | `ctest -R vpsdemo_testkit_throughput` |
| vpsdemo_testkit_dt_stability_test | dt | `ctest -R vpsdemo_testkit_dt_stability` |
| vpsdemo_testkit_dt_perf_test | dt | `ctest -R vpsdemo_testkit_dt_perf` |
| vpsdemo_testkit_stress_config_test | stress | `ctest -R vpsdemo_testkit_stress_config` |
| vpsdemo_testkit_stress_smoke | stress | `ctest -R vpsdemo_testkit_stress_smoke` |
| vpsdemo_sample_rules_test | unit | `ctest -R vpsdemo_sample` |
| vpsdemo_health_test | unit | `ctest -R vpsdemo_health` |
| vpsdemo_policy_test | unit | `ctest -R vpsdemo_policy` |
| vpsdemo_crash_recovery_test | unit | `ctest -R vpsdemo_crash_recovery` |
| vpsdemo_supervisor_integration_test | integration | `ctest -L integration` |
| vpsdemo_stress_test | stress | `ctest -L stress` |
| vpsdemo_dt_crash_recovery_test | dt | `ctest -L dt` |
| vpsdemo_codec_fuzz_smoke | fuzz | `ctest -L fuzz` |
| vpsdemo_testkit_codec_fuzz_smoke | fuzz | `ctest -L fuzz` |

Run all vpsdemo tests: `ctest -R vpsdemo`

Run by label:
- `ctest -L dt` - DT (Deterministic Testing) tests
- `ctest -L integration` - Integration tests
- `ctest -L stress` - Stress tests
- `ctest -L fuzz` - Fuzz smoke tests (requires Clang build)

Notes:
- Top-level builds default vpsdemo tests to ON via the root `CMakeLists.txt` and `tools/build_and_test.sh`.
- Setting `-DMEMRPC_ENABLE_FUZZ=ON` enables both memrpc fuzz and vpsdemo fuzz in top-level builds.

## Commit Guidelines

Follow the existing commit style:

- `feat: ...`
- `fix: ...`

Keep commits scoped. Before opening a PR, run the full build and `ctest`, summarize the behavior change, and note any intentionally excluded directories or follow-up work.
