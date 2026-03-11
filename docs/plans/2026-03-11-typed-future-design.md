# Typed Future Async API Design

## Goal
Introduce a framework-level typed async API so application code no longer sees untyped `RpcFuture`. The public API should return a `TypedFuture<Rep>` that performs owning decode at consumption time (`Wait/Then`), while the framework remains free to use 0-copy view decode internally.

## Scope
- Add a public `TypedFuture<Rep>` wrapper for `RpcFuture`.
- Add typed async entry points that return `TypedFuture<Rep>`.
- Update MiniRpc async client to return typed futures.
- Keep existing `RpcFuture` API for low-level use.
- No public view-based futures (internal view decode only).

## Non-Goals
- Replacing `RpcFuture` or `RpcClient::InvokeAsync`.
- Exposing view decode types to application code.
- Changing the dispatcher threading model.

## Architecture
- `TypedFuture<Rep>` is a thin wrapper that owns a `RpcFuture` and provides typed `Wait/WaitFor/Then`.
- Decoding happens in the caller context of `Wait/Then` (owning decode). No dispatcher-thread decode.
- `InvokeTypedAsync<Req, Rep>` returns `TypedFuture<Rep>` by wrapping a `RpcFuture`.
- MiniRpc async client returns `TypedFuture<Reply>`.

## Components
- New header: `include/memrpc/client/typed_future.h`
  - `template <typename Rep> class TypedFuture`
  - API: `Wait`, `WaitFor`, `Then`, `IsReady` (delegates to `RpcFuture`).
- Extend `include/memrpc/client/typed_invoker.h`
  - New function `InvokeTypedAsync<Req, Rep>` returning `TypedFuture<Rep>`.
  - Keep existing `InvokeTyped`, `WaitAndDecode`, `Then` for compatibility.
- Update `include/apps/minirpc/parent/minirpc_async_client.h`
  - Return `TypedFuture<EchoReply>`, `TypedFuture<AddReply>`, `TypedFuture<SleepReply>`.
- Update `src/apps/minirpc/parent/minirpc_async_client.cpp` to call `InvokeTypedAsync`.

## Data Flow & Error Handling
- `InvokeTypedAsync` encodes request, builds `RpcCall`, invokes `RpcClient::InvokeAsync`, wraps in `TypedFuture`.
- `TypedFuture::Wait/WaitFor` calls `RpcFuture::WaitAndTake` then decode payload to `Rep`.
- `TypedFuture::Then` registers a callback that decodes payload and passes `(StatusCode, Rep)`.
- Decode failure returns `StatusCode::ProtocolMismatch` with default-constructed `Rep`.
- `Wait/Then` are mutually exclusive, same semantics as `RpcFuture`.

## Testing
- New unit tests in `tests/memrpc/typed_future_test.cpp`:
  - `Wait` returns decoded reply.
  - `Then` decodes on callback and surfaces `ProtocolMismatch` on decode failure.
  - Mutual exclusion behavior mirrors `RpcFuture`.
- Extend `tests/apps/minirpc/minirpc_client_test.cpp` to validate typed async usage.

## Performance Notes
- Decoding in `Wait/Then` avoids dispatcher-thread CPU spikes.
- No additional allocations beyond decode output; payload ownership stays in `RpcReply`.
- Internal 0-copy view decode is unaffected and can continue for framework-only paths.

## Migration
- Applications should prefer typed async APIs (e.g., MiniRpc async client).
- Low-level code can still use `RpcFuture` if needed.

