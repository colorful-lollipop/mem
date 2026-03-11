# EnsureLiveSession Fast-Path Design

## Context
`EnsureLiveSession()` is called on the hot path (`InvokeAsync`, `Init`). It currently takes `reconnect_mutex` unconditionally and then `session_mutex`, which serializes callers even when the session is healthy.

## Goals
- Reduce contention on the hot path when the session is already live.
- Preserve existing correctness and reconnection serialization.
- Keep changes small and localized to `rpc_client.cpp`.

## Non-Goals
- Reworking session lifetime management or dispatch loops.
- Introducing RCU-style lifetime or complex lock-free structures.

## Proposed Approach (Recommended)
Add an atomic `session_live` flag that represents the invariant “`session` is valid and `slot_pool` is initialized for use.”

- Fast path: if `session_live.load(acquire)` is true, return `StatusCode::Ok` without taking any lock.
- Slow path: take `reconnect_mutex` to serialize reconnection, then `session_mutex` to check and (if needed) rebuild session state.
- After successful attach and initialization, set `session_live.store(true, release)` while holding `session_mutex`.
- On engine death or teardown, set `session_live.store(false, release)` before clearing `session`/`slot_pool`.

## Locking Invariants
- `reconnect_mutex` continues to serialize reconnection attempts.
- `session_mutex` remains the single guard for `session`, `slot_pool`, and related vectors.
- `session_live == true` implies `session.valid() == true` and `slot_pool != nullptr`.

## Data Flow Summary
- `InvokeAsync` -> `EnsureLiveSession`:
  - Fast path returns immediately when live.
  - Otherwise, reconnect under `reconnect_mutex` and `session_mutex` with a double-check.
- `HandleEngineDeath`:
  - Mark `session_live = false`, stop dispatcher, then tear down session data.

## Error Handling
Failures in `OpenSession`/`Attach` leave `session_live` false and return existing error codes. Logging behavior remains unchanged.

## Testing
No new tests planned initially; existing integration tests should continue to pass. If needed, add a focused unit test to assert that a live session bypasses `OpenSession()`.
