# Repository Guidelines

## Project Structure & Module Organization

Current mainline work focuses on the shared-memory RPC framework and the Virus Executor Service app.

- `memrpc/include/memrpc/`, `memrpc/src/`: framework code
- `virus_executor_service/include/{client,service,transport,testkit,ves}/`, `virus_executor_service/src/{app,client,service,transport,testkit,ves}/`: mainline application code
- `memrpc/tests/`: focused framework unit and integration tests
- `virus_executor_service/tests/`: app-level tests split by type: `unit/`, `integration/`, `stress/`, `dt/`, `fuzz/`
- `virus_executor_service/perf_baselines/`: app-owned perf/DT baselines
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
- `tools/build_and_test.sh --strict`
  Build with stricter warning flags enabled.
- `tools/build_and_test.sh --strict --werror`
  Escalate strict warnings into an error gate once the current warning backlog is reduced.
- `tools/build_and_test.sh --asan`
  Build into `build_asan/` with AddressSanitizer + UndefinedBehaviorSanitizer and leak detection.
- `tools/build_and_test.sh --tsan`
  Build into `build_tsan/` with ThreadSanitizer.
- `tools/build_and_test.sh --repeat until-fail:100 --test-regex memrpc_engine_death_handler_test`
  Re-run a high-risk test until it fails, useful for shutdown/deadlock race hunting.
- `tools/build_and_test.sh --timeout 90`
  Force a CTest per-test timeout from the script layer to avoid silent hangs.
- `tools/build_and_test.sh --fuzz`
  Enable both memrpc and virus_executor_service fuzz targets, then build and test them.
- `tools/build_and_test.sh --test-regex virus_executor_service`
  Build everything, then only run tests matching `virus_executor_service`.
- `tools/build_and_test.sh --label stress`
  Build everything, then only run `ctest -L stress`.
- `tools/ci_sweep.sh`
  Run the practical local verification matrix: strict full suite, ASan/UBSan full suite, TSan concurrency subset, then repeated shutdown-race regressions.
- `tools/push_gate.sh`
  Run the pre-push gate. This is the canonical gate for non-trivial code changes.
- `tools/push_gate.sh --deep`
  Run the heavier pre-push gate with longer repeat counts on concurrency-sensitive tests.
- `tools/build_and_test.sh --configure-only`
  Configure only.
- `tools/build_and_test.sh --build-only`
  Configure and build, but skip tests.
- `./build_ninja/virus_executor_service/virus_executor_service_supervisor`
  Run the Virus Executor Service supervisor after building.
- `./build_ninja/virus_executor_service/virus_executor_service_client`
  Run the standalone Virus Executor Service client after building.
- `./build_ninja/virus_executor_service/virus_executor_service_stress_client --threads 2 --iterations 100`
  Run the stress client directly.
- `./build_ninja/virus_executor_service/virus_executor_service_testkit_stress_runner`
  Run the testkit stress runner directly.
- Manual fallback: `cmake -S . -B build_ninja -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON`
- Manual build fallback: `cmake --build build_ninja --parallel`
- Manual test fallback: `ctest --test-dir build_ninja --output-on-failure --parallel`

### AI Quick Start

- If you are an AI agent with no repo context, start with `tools/build_and_test.sh --help`, then use `tools/build_and_test.sh`.
- For concurrency-sensitive changes in `memrpc/src/client/`, `memrpc/src/server/`, `memrpc/src/core/session.*`, or `virus_executor_service/tests/{dt,stress}/`, prefer:
  `tools/ci_sweep.sh`
- Before push or merge for any non-trivial change, run:
  `tools/push_gate.sh`
- For changes in lifecycle, shutdown, recovery, queueing, stress, DT, or fault injection paths, run:
  `tools/push_gate.sh --deep`
- Prefer the root build over ad-hoc per-module builds unless the task is isolated to `virus_executor_service/`.
- Default build directory is `build_ninja/`. Do not reuse old `build/` directories configured with another generator.
- This repo's meaningful tests use shared memory, Unix-domain sockets, and child processes. In sandboxed agent environments, `ctest` may fail with `shm_open ... errno=13` or registry start failures unless you request elevated permissions.
- In Codex-style sandboxes, request escalation before running full or integration-heavy test commands. Typical cases: `tools/build_and_test.sh`, `ctest --test-dir build_ninja ...`, or any `virus_executor_service_*integration*`, `stress`, `dt`, or shared-memory session tests.
- In Codex-style sandboxes, request escalation before `tools/ci_sweep.sh` and before any sanitizer or repeated CTest runs. These are intentionally heavier and may fork many child processes.

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
- Do not include headers from `memrpc/src/*` outside memrpc's own implementation/tests.
- For app headers, include from the flattened app prefixes only:
  - `client/*`
  - `service/*`
  - `transport/*`
  - `testkit/*`
  - `ves/*`

New code must follow these rules. Older code may be cleaned up opportunistically when touched.

## Logging

Use `virus_protection_service_log.h` only. Prefer Harmony-style calls such as `HILOGI(...)`, `HILOGW(...)`, and `HILOGE(...)`. Add logs only on important state changes, failures, or recovery paths.

## Testing Guidelines

Tests use GoogleTest via CMake. Name new tests by feature, for example `response_queue_event_test.cpp` or `testkit_client_test.cpp`.

- Keep tests focused on core behavior
- Add framework tests for transport, session, queue, and recovery logic
- Add app tests only for thin integration paths

Avoid broad end-to-end additions unless they protect a real regression.

For deadlock/race regressions:
- Add a focused GoogleTest that exercises the lifecycle directly.
- Add a repeatable `ctest --repeat until-fail:N` path for the affected test.
- Prefer explicit per-test timeouts over open-ended waits.
- If a test hangs, capture thread stacks from the stuck process before changing code.

### virus_executor_service Tests

The virus_executor_service module includes both GoogleTest unit tests and executable integration tests:

| Test | Type | Command |
|------|------|---------|
| virus_executor_service_session_service_test | unit | `ctest -R virus_executor_service_session` |
| virus_executor_service_heartbeat_test | unit | `ctest -R virus_executor_service_heartbeat` |
| virus_executor_service_codec_test | unit | `ctest -R virus_executor_service_codec` |
| virus_executor_service_testkit_headers_test | unit | `ctest -R virus_executor_service_testkit_headers` |
| virus_executor_service_testkit_codec_test | unit | `ctest -R virus_executor_service_testkit_codec` |
| virus_executor_service_testkit_service_test | unit | `ctest -R virus_executor_service_testkit_service` |
| virus_executor_service_testkit_client_test | unit | `ctest -R virus_executor_service_testkit_client` |
| virus_executor_service_testkit_dfx_test | unit | `ctest -R virus_executor_service_testkit_dfx` |
| virus_executor_service_testkit_backpressure_test | unit | `ctest -R virus_executor_service_testkit_backpressure` |
| virus_executor_service_testkit_async_pipeline_test | unit | `ctest -R virus_executor_service_testkit_async_pipeline` |
| virus_executor_service_testkit_baseline_test | unit | `ctest -R virus_executor_service_testkit_baseline` |
| virus_executor_service_testkit_latency_test | unit | `ctest -R virus_executor_service_testkit_latency` |
| virus_executor_service_testkit_throughput_test | perf | `ctest -R virus_executor_service_testkit_throughput` |
| virus_executor_service_testkit_dt_stability_test | dt | `ctest -R virus_executor_service_testkit_dt_stability` |
| virus_executor_service_testkit_dt_perf_test | dt | `ctest -R virus_executor_service_testkit_dt_perf` |
| virus_executor_service_testkit_stress_config_test | stress | `ctest -R virus_executor_service_testkit_stress_config` |
| virus_executor_service_testkit_stress_smoke | stress | `ctest -R virus_executor_service_testkit_stress_smoke` |
| virus_executor_service_sample_rules_test | unit | `ctest -R virus_executor_service_sample` |
| virus_executor_service_health_test | unit | `ctest -R virus_executor_service_health` |
| virus_executor_service_policy_test | unit | `ctest -R virus_executor_service_policy` |
| virus_executor_service_crash_recovery_test | unit | `ctest -R virus_executor_service_crash_recovery` |
| virus_executor_service_supervisor_integration_test | integration | `ctest -L integration` |
| virus_executor_service_stress_test | stress | `ctest -L stress` |
| virus_executor_service_dt_crash_recovery_test | dt | `ctest -L dt` |
| virus_executor_service_codec_fuzz_smoke | fuzz | `ctest -L fuzz` |
| virus_executor_service_testkit_codec_fuzz_smoke | fuzz | `ctest -L fuzz` |

Run all virus_executor_service tests: `ctest -R virus_executor_service`

Run by label:
- `ctest -L dt` - DT (Deterministic Testing) tests
- `ctest -L integration` - Integration tests
- `ctest -L stress` - Stress tests
- `ctest -L fuzz` - Fuzz smoke tests (requires Clang build)

Notes:
- Top-level builds default virus_executor_service tests to ON via the root `CMakeLists.txt` and `tools/build_and_test.sh`.
- Setting `-DMEMRPC_ENABLE_FUZZ=ON` enables both memrpc fuzz and virus_executor_service fuzz in top-level builds.

## Commit Guidelines

Follow the existing commit style:

- `feat: ...`
- `fix: ...`

Keep commits scoped. Before opening a PR, run the full build and `ctest`, summarize the behavior change, and note any intentionally excluded directories or follow-up work.

### Push Gate

Treat these as push conditions for meaningful code changes:

- Minimum push gate:
  `tools/push_gate.sh`
- Required for concurrency/lifecycle/recovery changes:
  `tools/push_gate.sh --deep`
- If a sandbox blocks shared memory, sockets, or child-process tests, request elevation rather than downgrading the gate.
- Do not claim a change is validated if only a single non-sanitized build was run.
