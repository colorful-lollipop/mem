# Repository Guidelines

## Project Structure & Module Organization

Current mainline work focuses on the shared-memory RPC framework and the minimal demo app.

- `include/memrpc/`, `src/core/`, `src/client/`, `src/server/`, `src/bootstrap/`: framework code
- `include/apps/minirpc/`, `src/apps/minirpc/`: minimal app used to validate cross-process RPC
- `tests/`: focused unit and integration tests; keep framework and app tests logically separated
- `demo/`: runnable examples
  - `memrpc_minirpc_demo`: minimal cross-process RPC demo
  - `vpsdemo/`: VPS (Virus Protection Service) demo application with supervisor, engine SA, and client
- `docs/`: architecture, porting notes, and implementation plans

Keep business-specific compatibility layers out of the framework. Applications should adapt their own legacy APIs.

## Build, Test, and Development Commands

- `cmake -S . -B build`: configure the project
- `cmake --build build`: build libraries, tests, and demos
- `ctest --test-dir build --output-on-failure`: run the active test suite (44 tests)
- `./build/demo/memrpc_minirpc_demo`: run the minimal cross-process demo
- `./build/demo/vpsdemo/vpsdemo_supervisor`: run VPS demo (supervisor + engine + client)
- `./build/demo/vpsdemo/vpsdemo_stress_client --threads 2 --iterations 100`: run stress test

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

Tests use GoogleTest via CMake. Name new tests by feature, for example `response_queue_event_test.cpp` or `minirpc_client_test.cpp`.

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
| vpsdemo_sample_rules_test | unit | `ctest -R vpsdemo_sample` |
| vpsdemo_health_test | unit | `ctest -R vpsdemo_health` |
| vpsdemo_policy_test | unit | `ctest -R vpsdemo_policy` |
| vpsdemo_crash_recovery_test | unit | `ctest -R vpsdemo_crash_recovery` |
| vpsdemo_supervisor_integration_test | integration | `ctest -L integration` |
| vpsdemo_stress_test | stress | `ctest -L stress` |
| vpsdemo_dt_crash_recovery_test | dt | `ctest -L dt` |

Run all vpsdemo tests: `ctest -R vpsdemo`

Run by label:
- `ctest -L dt` - DT (Deterministic Testing) tests
- `ctest -L integration` - Integration tests
- `ctest -L stress` - Stress tests

## Commit Guidelines

Follow the existing commit style:

- `feat: ...`
- `fix: ...`

Keep commits scoped. Before opening a PR, run the full build and `ctest`, summarize the behavior change, and note any intentionally excluded directories or follow-up work.
