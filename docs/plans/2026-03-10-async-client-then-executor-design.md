# Async-First Client Split + Then Executor Design

## Goal
Make async usage the primary client path, keep sync usage explicit via a separate client type, and add an optional executor for `Then` callbacks so async handlers can run off the dispatcher thread when desired.

## Non-Goals
- No protocol or shared-memory layout changes.
- No behavior changes to request/response semantics or timeouts beyond the client API split.
- No new global thread pool or scheduler built into the framework.

## Chosen Approach
- Keep `RpcClient` async-only (remove `InvokeSync`).
- Add `RpcSyncClient` as a thin wrapper that exposes `InvokeSync` and forwards the rest.
- Keep `RpcFuture` with `Then/IsReady/Wait/WaitFor/WaitAndTake` (no "compat-only" labeling).
- Add an optional per-call executor to `Then` for dispatching callbacks.
- In `typed_invoker.h`, provide a templated `Then<Rep>` that decodes and calls back with `(StatusCode, Rep)`.

## Architecture
- **Async Core:** `RpcClient` + `RpcFuture` with `InvokeAsync` and `Then` as the primary API.
- **Sync Wrapper:** `RpcSyncClient` wraps `RpcClient` and provides `InvokeSync`.
- **Typed Helpers:** `typed_invoker.h` contains async helpers and decoding utilities, keeping codec dependencies out of core client logic.

## API Changes
- `RpcClient`:
  - Remove `InvokeSync`.
  - Keep `InvokeAsync` unchanged.
- `RpcFuture`:
  - `void Then(std::function<void(RpcReply)> callback,
              RpcThenExecutor executor = {});`
  - `using RpcThenExecutor = std::function<void(std::function<void()>)>;`
- `typed_invoker.h`:
  - Add `template <typename Rep> void Then(RpcFuture future,
    std::function<void(StatusCode, Rep)> callback,
    RpcThenExecutor executor = {});`
  - Keep `InvokeTyped` and `InvokeTypedSync`.
- `RpcSyncClient` (new):
  - `StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);`
  - Forward `Init/Shutdown/SetBootstrapChannel/SetEventCallback/SetFailureCallback/GetRuntimeStats` to the inner `RpcClient`.

## Data Flow
1. `InvokeAsync` returns a `RpcFuture` with `ready=false`.
2. `Then(callback, executor)`:
   - If ready: execute via `executor` if provided; otherwise run on the calling thread.
   - If not ready: store `callback + executor` in state.
3. Dispatcher completion:
   - If `callback` present: move `callback + executor`, unlock, then dispatch (executor if set, else inline on dispatcher).
   - Otherwise `cv.notify_one()` for `Wait` path.
4. `typed_invoker.h::Then<Rep>` wraps decode logic and calls back with `(StatusCode, Rep)`.

## Error Handling
- Base `Then` passes full `RpcReply` to the callback unchanged.
- Typed `Then<Rep>`:
  - On non-`Ok` status: `cb(status, {})`.
  - On decode failure: `cb(ProtocolMismatch, {})`.
  - On success: `cb(Ok, decoded)`.
- `Wait/WaitFor/WaitAndTake` behavior unchanged.

## Component Changes
- `include/memrpc/client/rpc_client.h`:
  - Add `RpcThenExecutor` type.
  - Update `RpcFuture::Then` signature.
  - Add `RpcSyncClient` declaration.
  - Remove `RpcClient::InvokeSync`.
- `src/client/rpc_client.cpp`:
  - Implement executor-aware `Then`.
  - Move `InvokeSync` logic to `RpcSyncClient` implementation.
- `include/memrpc/client/typed_invoker.h`:
  - Rename `ThenDecode` to `Then<Rep>` and add executor parameter.
- `apps/minirpc`:
  - Keep `MiniRpcAsyncClient` as the primary example.
  - Convert sync wrapper to use `RpcSyncClient`.
  - Optionally rename `MiniRpcClient` to `MiniRpcSyncClient` (with alias if desired).

## Testing
- `RpcFuture`:
  - `Then` with executor dispatches via executor.
  - `Then` on already-ready future still dispatches via executor.
- `typed_invoker.h`:
  - `Then<Rep>` success path.
  - `Then<Rep>` decode failure returns `ProtocolMismatch`.
- `RpcSyncClient`:
  - Basic validation of `InvokeSync` path (uses `InvokeAsync` + wait budget).

## Risks and Mitigations
- **Risk:** Mixing `RpcClient`/`RpcSyncClient` usage might confuse users.
  - **Mitigation:** keep naming explicit and ensure examples emphasize async-first.
- **Risk:** Executor misuse could block completion.
  - **Mitigation:** document executor expectations; keep default inline behavior.

## Compatibility
- Wire protocol and shared memory layout unchanged.
- Sync behavior remains available via `RpcSyncClient` and `RpcFuture::Wait*`.
