# vpsdemo Refactor Design

## Goal
Make `demo/vpsdemo` a self-contained, cross-process demo that depends only on the memrpc framework. Provide both async and sync client APIs (sync is the external interface), keep existing demo behavior, and standardize logging on `HLOG*`.

## Context
- The current `demo/vpsdemo` has a working cross-process flow (registry + supervisor + engine + client) but uses protocol/types from `apps/vps`.
- `apps/vps` appears to be a legacy/in-process layer and is no longer desired for demo usage.

## Requirements
- `demo/vpsdemo` is independent of `apps/vps` and uses only framework headers (`memrpc/*`) plus its own demo headers.
- Provide both async and sync client APIs:
  - Async API returns `MemRpc::TypedFuture<Reply>`.
  - Sync API waits on the async future and returns `MemRpc::StatusCode` with decoded reply output.
- Keep demo behavior (supervisor spawns registry/engine/client, SCM_RIGHTS passes handles).
- Keep only base demo operations: `Init`, `ScanFile`, `UpdateFeatureLib`.
- Logging uses `virus_protection_service_log.h` macros (`HLOGI/HLOGW/HLOGE`). No `std::cout`/`std::cerr`.

## Non-Goals
- No expansion of VPS business functionality (behavior scan, analysis engine lifecycle, etc.).
- No changes to core framework architecture beyond demo integration.

## Proposed Architecture
### Layers (within demo)
1. **Framework Adapter Layer**
   - Registry client/server, bootstrap proxy/stub, SCM_RIGHTS transport.
   - Responsible for creating `memrpc::BootstrapHandles` for client and server.

2. **Demo Protocol/Codec Layer**
   - Define demo request/response structs for `Init`, `ScanFile`, `UpdateFeatureLib`.
   - Encode/decode helpers live in `demo/vpsdemo/include` + `demo/vpsdemo/src`.

3. **Demo Client Layer**
   - `VpsDemoAsyncClient`: wraps `memrpc::RpcClient` + `InvokeTypedAsync`.
   - `VpsDemoClient`: sync wrapper that calls `.Wait` on the typed future.

4. **Demo App Layer**
   - `vpsdemo_client`, `vpsdemo_engine_sa`, `vpsdemo_supervisor`.

### Data Flow
- Supervisor starts registry and engine SA; client queries registry for service socket.
- Client proxy receives `BootstrapHandles` via SCM_RIGHTS and builds `memrpc::RpcClient`.
- Async client issues typed calls; sync client waits and decodes.
- Engine SA creates `memrpc::RpcServer`, registers demo handlers, and serves requests.

### Error Handling and Logging
- Transport errors return `MemRpc::StatusCode` (and log on failures).
- Decode errors return `ProtocolMismatch` and log a single failure line.
- Log only state changes, failures, or recovery paths.

## Build and Cleanup
- Remove `include/apps/vps` and `src/apps/vps` from build and repo.
- Update top-level and `demo/vpsdemo` CMake lists to reference new demo sources only.

## Testing/Validation
- Run the demo binaries end-to-end (supervisor drives full flow).
- No new tests are required for this refactor.
