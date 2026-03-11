# MemRPC Recovery Policy Design

## Background
Vpsdemo currently relies on `EngineDeathHandler` for restart after engine death, but it does not react to failure callbacks such as `ExecTimeout`. Minirpc has a `FailureMonitor` helper but it is only used in tests. The user requirement is to make recovery decisions consistent across three signals: idle, engine death, and task failure. The design should support immediate restart on the first exec-timeout, with a configurable delay between `CloseSession` and `OpenSession`.

## Goals
- Provide a **unified recovery decision type** for idle, engine-death, and failure callbacks.
- Allow **exec-timeout immediate restart** with a configurable delay.
- Keep restart logic centralized inside `RpcClient` so callers only provide policy.
- Allow VpsClient to configure recovery behavior with simple options.

## Non-Goals
- No changes to server-side semantics or supervisor process behavior.
- No new persistent metrics or dashboards.
- No backward compatibility guarantees (breaking API changes allowed).

## Proposed API (Approach 2 — RecoveryPolicy)
Replace the three independent setters with a single policy object that contains three handlers returning the same decision type.

### New Types
- `enum class RecoveryAction { Ignore, Restart }`
- `struct RecoveryDecision { RecoveryAction action; uint32_t delay_ms; }`
- `struct RecoveryPolicy`
  - `std::function<RecoveryDecision(const RpcFailure&)> onFailure;`
  - `std::function<RecoveryDecision(uint64_t idle_ms)> onIdle;`
  - `std::function<RecoveryDecision(const EngineDeathReport&)> onEngineDeath;`

### API Surface
- Replace:
  - `SetFailureCallback`, `SetIdleCallback`, `SetEngineDeathHandler`
- With:
  - `SetRecoveryPolicy(RecoveryPolicy policy)`

Callers that only want notifications can return `{Ignore, 0}`.

## Recovery Flow
### Engine Death
1. Detect session break and build `EngineDeathReport` (existing logic).
2. Call `policy.onEngineDeath(report)`.
3. If `Ignore`: fail pending as `PeerDisconnected` and stop.
4. If `Restart`: schedule restart thread with `delay_ms` and replay safe calls.

### Failure / Idle (Forced Restart)
1. Invoke `policy.onFailure` or `policy.onIdle`.
2. If `Ignore`: no recovery.
3. If `Restart`: trigger **forced restart**:
   - Snapshot replay-safe vs poison-pill requests (same logic as engine-death path).
   - Call `bootstrap->CloseSession()` to cleanly reset the server session.
   - Sleep `delay_ms`.
   - Re-open session via `EnsureLiveSession()` and replay safe requests.
   - Poison-pill requests get `CrashedDuringExecution` and are surfaced as failures.

### Concurrency / Re-entrancy
- Only one restart may be active at a time.
- A new `restart_pending` flag in `RpcClient::Impl` gates restart scheduling.
- Intentional `CloseSession()` should not trigger the engine-death callback (guard with a flag).

## VpsClient Policy Defaults
Add `VpsClient::Options` (or similar) with:
- `execTimeoutRestartDelayMs` (default e.g. 200ms)
- `engineDeathRestartDelayMs` (default e.g. 200ms)
- `idleRestartDelayMs` (default 0; idle ignored unless explicitly set)

Default policy:
- `onFailure`: if `failure.status == ExecTimeout`, return `Restart` with `execTimeoutRestartDelayMs`, else `Ignore`.
- `onEngineDeath`: log report and return `Restart` with `engineDeathRestartDelayMs`.
- `onIdle`: return `Ignore` unless `idleRestartDelayMs > 0`.

## Migration
- Update all call sites/tests from the old setters to `SetRecoveryPolicy`.
- Minirpc tests that used `SetFailureCallback` will provide a policy with `onFailure` returning `Ignore` (or custom behavior where required).
- Vpsdemo clients will be updated to set policy via VpsClient options.

## Testing Strategy
- Update existing tests that assert failure callbacks to use the new policy.
- Add a focused test verifying `ExecTimeout` triggers `Restart` with correct delay.
- Validate that forced restart path calls `CloseSession` + `OpenSession` and replays safe requests.

## Open Questions (Resolved)
- Restart should be **immediate on first exec-timeout** (no threshold).
- Restart uses `CloseSession` then `OpenSession` with configurable delay.
- Breaking API changes are acceptable.
