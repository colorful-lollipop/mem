# MemRPC & Virus Executor Service

A **shared-memory RPC framework** for C++17, paired with a reference application that demonstrates high-throughput, low-latency inter-process communication on **Linux** and **OpenHarmony**.

> **tl;dr** — `memrpc` moves small payloads through lock-free shared-memory rings. Large or control-plane requests transparently fall back to a synchronous `AnyCall` side channel. Recovery, session lifecycle, and backpressure are handled by the framework so business code stays thin.
> On **OpenHarmony**, the data plane remains shared-memory + `eventfd` while the control plane is bridged through the **SystemAbility (SA)** layer.

---

## Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
- [Supported Platforms](#supported-platforms)
- [Architecture at a Glance](#architecture-at-a-glance)
- [Quick Start](#quick-start)
  - [Prerequisites](#prerequisites)
  - [Build](#build)
  - [Run Tests](#run-tests)
  - [Run the Demo](#run-the-demo)
- [Project Structure](#project-structure)
- [Testing Matrix](#testing-matrix)
- [Documentation](#documentation)
- [OpenHarmony Deployment](#openharmony-deployment)
- [License](#license)

---

## Overview

This repository contains two main parts:

1. **`memrpc/`** — A reusable shared-memory RPC framework.
   - Fixed-size ring entries (inline payload, no external slots).
   - Lock-free request/response rings plus eventfd-based wake-up.
   - Typed C++ facades (`TypedFuture`, `InvokeTypedSync`) with automatic encode/decode.
   - Unified client-side recovery state machine (engine-death detection, cooldown, idle-close, session reopen).

2. **`virus_executor_service/`** — A reference application (Virus Executor Service, or *VES*) built on top of `memrpc`.
   - Demonstrates how to register business handlers to both the MemRPC fast path and the `AnyCall` fallback path without writing duplicate transport logic.
   - Includes a complete **testkit** (`Echo`, `Add`, `Sleep`, fault-injection handlers) so you can reason about framework behavior through minimal, deterministic examples.

The framework is intentionally **platform-agnostic** at its core. The same `RpcClient` / `RpcServer` / `Session` code runs on Linux development workstations and on OpenHarmony devices; only the **bootstrap adapter** (how the client discovers the server and exchanges file descriptors) is platform-specific.

---

## Key Features

- ⚡ **Zero-copy fast path** for small payloads via shared-memory rings  
- 🔀 **Dual-path transport** — automatic fallback to `AnyCall` when a request exceeds the inline entry limit  
- 🛡️ **Conservative backpressure** — bounded rings, bounded worker queues; no silent unbounded buffering  
- 🔄 **Client-side recovery** — `RpcClient` owns session lifecycle, health checks, and replay policy; business code only supplies a `RecoveryPolicy`  
- 🧪 **Extensive test surface** — unit, integration, deterministic (DT), stress, and fuzz tests  
- 📱 **OpenHarmony-native architecture** — SystemAbility control plane + shared-memory data plane; core framework requires **zero** changes when porting to OpenHarmony  

---

## Supported Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| **Linux (x86_64 / AArch64)** | ✅ Primary dev & test | Full CMake build, all tests, supervisor demo |
| **OpenHarmony** | ✅ Target deployment | SA bootstrap adapter + `init` child-process management; see [OpenHarmony Deployment](#openharmony-deployment) |

---

## Architecture at a Glance

```
┌─────────────────┐      typed API       ┌─────────────────┐
│   VesClient     │ ◄──────────────────► │  ScanFile()     │
│  (facade +      │                      │  Heartbeat()    │
│   recovery)     │                      │  AnyCall()      │
└────────┬────────┘                      └─────────────────┘
         │
         │  small payload ──► RpcClient ──► Request Ring  ──► RpcServer ──► handler
         │  large payload ──► AnyCall() ──► control-plane proxy
         │
         ▼
┌─────────────────┐
│  memrpc Session │  ←  shared memory, eventfd, attach/validate
└─────────────────┘
```

**Core design boundaries:**

- `memrpc::Session` — mmap, ring cursors, protocol version check, client attachment.
- `memrpc::RpcClient` — pending table, timeouts, response loop, recovery state machine.
- `memrpc::RpcServer` — dispatcher, high/normal priority executors, response writer.
- `VesEngineService` — pure business logic (e.g. `ScanFile`).
- `EngineSessionService` — wires business handlers into both `RpcServer` and `AnyCall`.

For a deep-dive, see [`docs/architecture.md`](docs/architecture.md).

---

## Quick Start

### Prerequisites

- **Linux** development environment
- **Clang** toolchain (`clang`, `clang++`)
- **CMake** ≥ 3.16
- **Ninja** (recommended)
- **GoogleTest** (required for tests)

> The build system enforces Clang. GCC is not supported for the mainline configuration.

### Build

The canonical zero-knowledge entry point:

```bash
./tools/build_and_test.sh
```

This configures, builds, and runs the full test suite into `build_ninja/`.

Other useful variants:

```bash
# Clean rebuild
./tools/build_and_test.sh --clean

# Strict warnings
./tools/build_and_test.sh --strict

# AddressSanitizer + UBSan
./tools/build_and_test.sh --asan

# ThreadSanitizer
./tools/build_and_test.sh --tsan

# Build only, skip tests
./tools/build_and_test.sh --build-only
```

Manual fallback (if you prefer direct CMake):

```bash
cmake -S . -B build_ninja -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
cmake --build build_ninja --parallel
```

### Run Tests

Run everything:

```bash
ctest --test-dir build_ninja --output-on-failure --parallel
```

Run by module or label:

```bash
# Framework tests only
ctest --test-dir build_ninja -R memrpc_

# Application tests only
ctest --test-dir build_ninja -R virus_executor_service_

# Stress tests
ctest --test-dir build_ninja -L stress

# DT (deterministic) tests
ctest --test-dir build_ninja -L dt

# Integration tests
ctest --test-dir build_ninja -L integration

# Fuzz smoke tests
ctest --test-dir build_ninja -L fuzz
```

Pre-push validation gates:

```bash
# Standard gate
./tools/push_gate.sh

# Deep gate (concurrency / lifecycle / recovery changes)
./tools/push_gate.sh --deep
```

> **Note:** Many tests use `shm_open`, `mmap`, Unix-domain sockets, and child processes. In sandboxed CI environments you may need elevated permissions for the full suite to pass.

### Run the Demo

After building:

```bash
# Supervisor (service lifecycle demo)
./build_ninja/virus_executor_service/virus_executor_service_supervisor

# Standalone client
./build_ninja/virus_executor_service/virus_executor_service_client

# Stress client
./build_ninja/virus_executor_service/virus_executor_service_stress_client --threads 2 --iterations 100

# Testkit stress runner
./build_ninja/virus_executor_service/virus_executor_service_testkit_stress_runner
```

---

## Project Structure

```
memrpc/
├── include/memrpc/core/      # Protocol, types, codec, bootstrap abstraction
├── include/memrpc/client/    # RpcClient, TypedFuture, typed invoker
├── include/memrpc/server/    # RpcServer, handler interfaces
├── src/core/                 # Session, byte codec, shared-memory layout
├── src/client/               # RpcClient implementation (threads, recovery)
├── src/server/               # RpcServer implementation (dispatcher, workers)
├── src/bootstrap/            # DevBootstrapChannel (local dev / testing)
└── tests/                    # Framework unit & integration tests

virus_executor_service/
├── include/client/           # VesClient public API
├── include/service/          # System-ability shell interfaces
├── include/transport/        # Control-plane proxy, registry
├── include/ves/              # Business protocol (opcodes, typed structs)
├── include/testkit/          # Testkit headers
├── src/client/               # VesClient implementation
├── src/service/              # VirusExecutorService, EngineSessionService
├── src/transport/            # VesBootstrapChannel, registry backend
├── src/testkit/              # Testkit service & clients
├── src/app/                  # Supervisor, client, stress, DT executables
├── tests/unit/               # Unit tests
├── tests/integration/        # Integration tests
├── tests/dt/                 # Deterministic / recovery tests
├── tests/stress/             # Stress / throughput tests
├── tests/fuzz/               # Fuzz targets
└── perf_baselines/           # Performance baselines

docs/                         # Architecture, testing guide, porting notes
tools/                        # build_and_test.sh, push_gate.sh, ci_sweep.sh
```

---

## Testing Matrix

| Category | Typical Targets | When to Run |
|----------|-----------------|-------------|
| **Unit** | `memrpc_*_test`, `virus_executor_service_codec_test`, `virus_executor_service_policy_test` | Every change |
| **Integration** | `virus_executor_service_supervisor_integration_test` | Changes to `src/app/`, transport, or registry |
| **DT** | `virus_executor_service_crash_recovery_test`, `memrpc_dt_stability_test` | Recovery, lifecycle, health-check changes |
| **Stress** | `virus_executor_service_stress_test`, `testkit_throughput_test` | Concurrency, scheduling, or performance changes |
| **Fuzz** | `virus_executor_service_codec_fuzz_smoke` | Codec, protocol struct, or parser changes |

For a detailed guide on *which tests protect which source files*, see [`docs/testing_guide.md`](docs/testing_guide.md).

---

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — How the framework and application layers fit together.
- [`docs/testing_guide.md`](docs/testing_guide.md) — Test philosophy, representative tests, and daily validation matrix.
- [`docs/demo_guide.md`](docs/demo_guide.md) — How to build, run, and verify the demo.
- [`docs/porting_guide.md`](docs/porting_guide.md) — How to adapt `memrpc` for a new business domain.
- [`docs/recovery_ownership.md`](docs/recovery_ownership.md) — Who owns what in the recovery path (`RpcClient` vs `VesClient`).
- [`docs/sa_integration.md`](docs/sa_integration.md) — OpenHarmony SystemAbility integration guide.

---

## OpenHarmony Deployment

`memrpc` is designed so that the **framework core** and **business logic** have **zero** dependency on OS-specific transport details.

When deploying on OpenHarmony:

1. **Keep `memrpc` core unchanged** — `Session`, `RpcClient`, `RpcServer`, rings, and eventfd logic require no porting.
2. **Keep business handlers unchanged** — `VesEngineService` and the typed protocol stay exactly the same.
3. **Plug in the SA bootstrap adapter** — replace `DevBootstrapChannel` with a production `VesBootstrapChannel` that uses `GetSystemAbility` / `LoadSystemAbility` and exchanges file descriptors over Binder.
4. **Let `init` manage the engine process** — child-process lifecycle moves from the local supervisor demo to the OpenHarmony `init` subsystem.

In other words, OpenHarmony support is **not a downstream fork**; it is a **swap of the bootstrap adapter** while the shared-memory data plane remains intact.

For the full SA integration design, see [`docs/sa_integration.md`](docs/sa_integration.md) and [`docs/harmony_sa_retrofit_plan.md`](docs/harmony_sa_retrofit_plan.md).

---

## License

Please refer to the repository's top-level license file for the terms governing this project.

---

*Happy hacking! If you run into shared-memory permission issues in sandboxed environments, make sure your container or CI runner has the required capabilities for `shm_open`, `mmap`, and Unix-domain sockets.*
