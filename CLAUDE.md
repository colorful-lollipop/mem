# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
cmake -S . -B build                              # configure
cmake --build build                              # build everything
ctest --test-dir build --output-on-failure        # run all tests
ctest --test-dir build --output-on-failure -R <pattern>  # run single test (e.g. -R slot_pool)
./build/vpsdemo/vpsdemo_supervisor                # run the mainline VPS demo
```

CMake is the source of truth during active development. GN (`BUILD.gn`) is maintained alongside for future HarmonyOS builds.

## Project Overview

MemRPC is a shared-memory + eventfd inter-process RPC framework for Linux/HarmonyOS. It enables a parent process (client) to communicate with a forked child process (server) via shared memory rings and eventfd notifications. Originally designed to split a virus protection service into isolated dual processes.

## Architecture

**Layer structure:**

- **Framework** (`memrpc/include/memrpc/`, `memrpc/src/`): Platform-agnostic RPC transport
- **Application** (`vpsdemo/include/vpsdemo/`, `vpsdemo/src/{app,client,service,transport,testkit,ves}/`): Mainline VPS app with `ves` protocol, bootstrap/registry integration, and `testkit` RPCs
- **Tests** (`memrpc/tests/`, `vpsdemo/tests/`): Framework tests plus app-owned unit/integration/stress/DT/fuzz coverage

**Key abstractions:**

- `IBootstrapChannel` — platform abstraction for process creation and shared memory setup. Implementations: `PosixDemoBootstrapChannel` (fork-based, Linux dev) and `SaBootstrapChannel` (HarmonyOS SA)
- `Session` — manages shared memory layout, ring buffers, slot pools, and eventfd channels
- `RpcClient` / `RpcFuture` — parent-side async/sync call API
- `RpcServer` / `RpcHandler` — child-side request dispatch and handler registration

**Shared memory layout:** Header → dual request rings (high-priority 64 entries + normal 256 entries) → response ring (256 entries) → request slot pool (128 × 4KB) → response slot pool. Five eventfd channels handle notifications (high req, normal req, response, req credit, resp credit).

**Namespace conventions:** Framework code lives under `memrpc::`. Application code uses `OHOS::Security::VirusProtectionService::` with `namespace MemRpc = ::memrpc;` alias.

## Coding Conventions

- C++17, 4-space indentation
- `UpperCamelCase` for types/functions/methods; `lowerCamelCase` for variables/parameters; `ALL_CAPS` for constants/macros
- Simple explicit code over template-heavy or macro-heavy abstractions
- Framework headers use layered paths: `memrpc/core/*`, `memrpc/client/*`, `memrpc/server/*`
- Logging: use `virus_protection_service_log.h` macros (`HILOGI`, `HILOGW`, `HILOGE`, `HILOGD`). Log only on state changes, failures, or recovery — never in hot paths
- Keep business-specific compatibility layers out of the framework

## Commit Style

Conventional commits: `feat:`, `fix:`, `docs:`, `style:`, `chore:`. Keep commits scoped and focused. Run full build + `ctest` before opening a PR.

## Testing

GoogleTest via CMake. Framework tests live in `memrpc/tests/`; vpsdemo and testkit tests live in `vpsdemo/tests/`. Name tests by feature (e.g. `slot_pool_test.cpp`, `testkit_client_test.cpp`). Keep tests focused on core behavior; avoid broad e2e tests unless protecting a real regression.

## Dependencies

Runtime: none beyond C++17 stdlib and POSIX APIs. Test-time: Google Test. `third_party/ohos_sa_mock/` provides stub HarmonyOS SA for Linux development.
