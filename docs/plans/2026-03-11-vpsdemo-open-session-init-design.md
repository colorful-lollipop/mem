# vpsdemo OpenSession Synchronous Init Design

**Date:** 2026-03-11

## Goal
Make `OpenSession` the single synchronous engine-initialization entry point for the vpsdemo SA path. The SA class is named `VirusExecutorService`. `AcceptLoop` remains a transport-layer mock and delegates real session creation to a server-side service object. The client no longer calls `InitEngine`.

## Scope
- Add a session provider service in `vpsdemo_engine_sa` that performs synchronous initialization and returns `BootstrapHandles`.
- Keep `AcceptLoop` limited to socket/SCM_RIGHTS transport; it calls the provider for real `OpenSession` logic.
- Remove client-side `InitEngine` usage; retain RPC handler for compatibility.

## Non-Goals
- No async initialization or wait/timeout logic.
- No protocol redesign or new transport format.
- No changes to memrpc core architecture beyond demo integration.

## Architecture
- `EngineSessionService` (server-side) owns real `OpenSession`:
  - Synchronously initializes the engine (once).
  - Returns `BootstrapHandles` from the same `OpenSession` call that triggers initialization.
- `VirusExecutorService` (renamed SA class; implements `IVpsBootstrap`) is transport-only:
  - `AcceptLoop()` calls `provider->OpenSession(&handles)`.
  - On success, sends fds + metadata via SCM_RIGHTS.
- `VpsDemoService` initialization is driven by `EngineSessionService` rather than the client `InitEngine` RPC.

## Components
- **New:** `demo/vpsdemo/include/vps_session_service.h`
- **New:** `demo/vpsdemo/src/vps_session_service.cpp`
  - `VpsSessionProvider` interface: `OpenSession` / `CloseSession`.
  - `EngineSessionService` implementation:
    - Holds `memrpc::PosixDemoBootstrapChannel`.
    - Performs synchronous `Initialize()` once.
    - Returns `BootstrapHandles` directly from the first `OpenSession`, avoiding a throwaway open/close.
- **Rename/Update:** `demo/vpsdemo/include/vps_bootstrap_stub.h` -> `demo/vpsdemo/include/virus_executor_service.h`
- **Rename/Update:** `demo/vpsdemo/src/vps_bootstrap_stub.cpp` -> `demo/vpsdemo/src/virus_executor_service.cpp`
  - Inject `VpsSessionProvider`.
  - `AcceptLoop()` calls provider `OpenSession()` then sends fds.
- **Update:** `demo/vpsdemo/src/vpsdemo_engine_sa.cpp`
  - Construct `EngineSessionService`.
  - Inject into `VirusExecutorService`.
- **Update:** `demo/vpsdemo/include/vpsdemo_service.h`
- **Update:** `demo/vpsdemo/src/vpsdemo_service.cpp`
  - Add `Initialize()` (synchronous, idempotent).
  - `DemoInit` handler may call `Initialize()` to remain compatible.
- **Update:** `demo/vpsdemo/src/vpsdemo_client.cpp`
  - Remove `InitEngine()` call.
 - **Unchanged names:** `IVpsBootstrap`, `VPS_BOOTSTRAP_SA_ID`, `VpsBootstrapProxy`.

## Data Flow
1. `VpsClient::Init()` -> `RpcClient::Init()` -> `bootstrap->OpenSession()`.
2. `VpsBootstrapProxy` connects and sends the open command.
3. `VirusExecutorService::AcceptLoop()` calls `provider->OpenSession(&handles)`.
4. `EngineSessionService::OpenSession()` calls `bootstrap->OpenSession(handles)` on first use, returns those handles, then initializes the engine once using `serverHandles()`.
5. `VirusExecutorService` sends handles via SCM_RIGHTS.
6. `RpcClient` attaches and processes RPCs; client no longer calls `InitEngine`.

## Error Handling
- If initialization fails, `OpenSession()` returns a failure status and stub does not send fds.
- `InitEngine` RPC remains but becomes a no-op/idempotent helper for compatibility.

## Testing & Verification
- Run the vpsdemo supervisor flow and verify `ScanFile`/`UpdateFeatureLib` succeed without client `InitEngine`.
- Confirm initialization happens once on first `OpenSession` and failures are logged.
