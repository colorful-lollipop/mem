# VPS Demo SA Mock Design

**Date:** 2026-03-11

## Goal
Build a runnable multi-process VPS demo that simulates HarmonyOS SA discovery and death handling while keeping the data plane on memrpc shared memory + eventfd. The demo must compile independently and run on Linux using `ohos_sa_mock` with cross-process semantics.

## Scope
- Add a standalone demo under `demo/vpsdemo/` that builds three executables: supervisor, engine SA, and client.
- Extend `ohos_sa_mock` with an optional cross-process registry client mode to support `GetSystemAbility/LoadSystemAbility/UnloadSystemAbility` over a Unix domain socket.
- Implement a VPS bootstrap SA interface that exchanges `BootstrapHandles` over a Unix domain socket using `SCM_RIGHTS`.
- Demonstrate death recipient and restart after `UpdateFeatureLib` (main process triggers restart).

## Non-Goals
- Full Binder IPC or MessageParcel support.
- Generic multi-service SA registry (demo focuses on the VPS bootstrap service).
- Changes to top-level repo build configuration (must not reference `ohos_sa_mock` there).

## Architecture

### Processes
1. **`vpsdemo_supervisor`** (resident)
   - Hosts the SA registry socket.
   - Spawns and restarts `vpsdemo_engine_sa` on demand.
   - Optionally spawns `vpsdemo_client` for a one-command demo run.

2. **`vpsdemo_engine_sa`**
   - Implements `SystemAbility` + `IRemoteStub<IVpsBootstrap>`.
   - Exposes a Unix domain socket for bootstrap control calls.
   - Owns `memrpc::RpcServer` + `VirusEngineService`.
   - Uses `PosixDemoBootstrapChannel` internally to create SHM + eventfd.

3. **`vpsdemo_client`**
   - Uses `SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager()`.
   - Calls `GetSystemAbility(1251)` and `iface_cast<IVpsBootstrap>`.
   - Uses `memrpc::SaBootstrapChannel` to `OpenSession` and perform VPS calls.
   - Installs a death recipient for restart handling.

### Data Plane vs Control Plane
- **Data plane:** memrpc shared memory rings + eventfd (unchanged).
- **Control plane:** SA mock registry + bootstrap socket using Unix domain sockets.

## SA Mock Registry (ohos_sa_mock extension)

### Mode Selection
- Default: existing in-process registry.
- Remote: enabled when env var `OHOS_SA_MOCK_REGISTRY_SOCKET` is set.

### Registry Protocol (Unix domain socket)
- Request/response with a small binary header: `op`, `sa_id`, `payload_len`.
- Ops:
  - `REGISTER(sa_id, service_socket_path)`
  - `GET(sa_id)`
  - `LOAD(sa_id)` (start if absent)
  - `UNLOAD(sa_id)` (stop and notify death)
- Response: `err_code` + optional `service_socket_path`.

### Client Behavior
- `SystemAbilityManagerClient` uses remote registry when env var is set.
- `GetSystemAbility` creates a local `IRemoteObject` and attaches a proxy broker `VpsBootstrapProxy` built from `service_socket_path`.
- `LoadSystemAbility` triggers supervisor to start engine if needed.
- `UnloadSystemAbility` requests supervisor to stop engine.

### Death Recipient Semantics
- `VpsBootstrapProxy` monitors its service socket (poll/recv in a small thread).
- On disconnect, it calls `IRemoteObject::NotifyRemoteDiedForTest()`, which triggers registered death recipients.

## VPS Bootstrap SA Interface

### Interface
A demo interface defined in `demo/vpsdemo/include/ves_bootstrap_interface.h`:
- `StatusCode OpenSession(BootstrapHandles* handles)`
- `StatusCode CloseSession()`

### Handle Exchange
- `OpenSession` returns the 6 fds via `SCM_RIGHTS` on the service socket.
- Metadata (`protocol_version`, `session_id`) is sent in the payload.
- Client validates fd count and metadata before attaching the session.

## Restart Flow (Update Feature)
1. Client calls `UpdateFeatureLib()` via memrpc.
2. Client then calls `UnloadSystemAbility(1251)`.
3. Supervisor terminates engine SA and removes it from registry.
4. Client calls `LoadSystemAbility(1251)` to restart.
5. `SaBootstrapChannel` opens a new session; death callback fires once for the old session.

This keeps “feature update on main process, engine reload on restart” semantics.

## Build & Run
- Standalone CMake under `demo/vpsdemo/` builds the three executables.
- It compiles:
  - memrpc core sources (excluding `src/bootstrap/sa_bootstrap.cpp`)
  - apps/vps sources
  - ohos_sa_mock sources (with remote registry client)
  - demo-specific bootstrap and proxy code

Commands:
```
cmake -S demo/vpsdemo -B demo/vpsdemo/build
cmake --build demo/vpsdemo/build
```

## Testing & Verification
- Manual run sequence:
  1. Start `vpsdemo_supervisor`.
  2. It spawns engine SA and client.
  3. Client performs `Init -> ScanFile -> UpdateFeatureLib -> ScanFile`.
  4. Verify a death callback is logged once and the second scan succeeds.
- Optional unit tests for:
  - registry protocol encode/decode
  - `SCM_RIGHTS` handle transfer
  - death recipient triggering on socket close

