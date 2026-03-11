# Server Executor Decoupling Design

## Background
`RpcServer` currently owns an internal `WorkerPool` in `src/server/rpc_server.cpp` for both high/normal request lanes. This hard-codes the threading model inside the server and makes it difficult to reuse or integrate with other execution frameworks.

Client-side already exposes a lightweight executor hook (`RpcThenExecutor`) for `Then` callbacks. We want a unified internal executor concept while preserving the server's backpressure behavior.

## Goals
- Decouple server request execution from the built-in thread pool.
- Preserve existing backpressure semantics (only drain rings when executor has capacity).
- Keep default behavior unchanged when no custom executor is supplied.
- Introduce a common internal executor abstraction that can be reused later.

## Non-Goals
- Adopting C++23 `std::execution` executors (C++17 only).
- Changing request scheduling priorities or ring semantics.
- Rewriting the dispatcher loop or response writer logic.

## Proposed Approach (Selected)
### 1) Add a framework `TaskExecutor` interface
A small interface that can submit tasks and expose capacity/backpressure:
- `bool TrySubmit(std::function<void()> task)`
- `bool HasCapacity() const`
- `bool WaitForCapacity(std::chrono::milliseconds timeout)`
- `void Stop()`

This captures the existing drain/pressure behavior without forcing a specific implementation.

### 2) Default `ThreadPoolExecutor`
Wrap the current `WorkerPool` logic into a `ThreadPoolExecutor` that matches today’s semantics:
- queue capacity tied to thread count
- `HasCapacity()` and `WaitForCapacity()` behave exactly as current `WorkerPool`
- tasks executed by worker threads

### 3) Integrate into `RpcServer`
- Extend `ServerOptions` to accept `std::shared_ptr<TaskExecutor>` for `high_executor` and `normal_executor`.
- If not provided, `RpcServer::Start()` constructs default `ThreadPoolExecutor` instances using the existing `high_worker_threads` / `normal_worker_threads` options.
- Dispatcher uses `TaskExecutor` capacity checks and waits to preserve backpressure.

### 4) Unify with client executor concept
Keep `RpcThenExecutor` as the public client API, but add an adapter utility to wrap it as a `TaskExecutor` if needed by shared infrastructure. This unifies the internal executor story without breaking client code.

## Data Flow Impact
- **Before**: dispatcher drains ring directly into `WorkerPool`.
- **After**: dispatcher drains ring into `TaskExecutor` (default `ThreadPoolExecutor`).
- Request execution (`ProcessEntry`) stays unchanged.

## Error Handling
- If a custom executor refuses tasks (no capacity), dispatcher waits as today.
- If executor is missing or stopped unexpectedly, treat as internal error and break the session (same severity as current pool failure).

## Testing
- Existing server tests should continue to pass with default executors.
- Add a test executor in tests to verify:
  - capacity gating prevents ring drain until capacity returns
  - custom executor receives tasks

## Risks
- Mis-implemented executors could break backpressure; provide clear interface contract and tests.

## Outcome
Server execution is decoupled and extensible while keeping the default behavior unchanged.
