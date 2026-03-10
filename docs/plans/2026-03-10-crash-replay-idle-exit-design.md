# Crash Replay Classification And Idle Exit Design

**Goal:** After server crash, classify each pending request as safe-to-replay vs maybe-executed (“poison”), and provide framework-level idle auto-exit that is configurable but optional.

**Context:**
- Server crash is expected; client must not crash but must be able to decide replay/skip per request.
- New session is allowed to use a new shm + session_id (no forced reuse).
- Current runtime state is stored in shared `SlotRuntimeState`, but crash-time classification is not defined.

---

## Requirements
- Distinguish between:
  - **SafeToReplay:** known not executed (client can auto-replay if app allows).
  - **MaybeExecuted:** could have executed (poison); app must decide.
- Avoid adding new IPC channels unless absolutely necessary.
- Provide idle auto-exit at framework level, with runtime config; default disabled.
- Preserve simple bootstrap model: `OpenSession()`/`CloseSession()` only.

## Non-Goals
- Exactly-once semantics.
- Automatic replay in framework without app decision.
- Reusing old shm after crash.

---

## Proposed Architecture

### 1) Slot Runtime State Is the Source of Truth
Reuse and formalize `SlotRuntimeStateCode` in shared memory as the crash classifier:
- `Admitted` — client wrote slot, not yet enqueued
- `Queued` — pushed into request ring
- `Executing` — worker picked up and started
- `Responding` — server preparing response
- `Ready` — response published
- `Consumed` — client consumed and released

**Rule:** runtime state only moves forward; each transition updates `last_heartbeat_mono_ms`.

### 2) Crash Classification Policy
When session death is detected (death callback or bootstrap error):
1. Stop new submissions.
2. Optionally drain response ring for a short deadline (reduce false “poison”).
3. Snapshot each pending slot’s runtime state and classify:
   - `Admitted` / `Queued` → **SafeToReplay**
   - `Executing` / `Responding` / `Ready` → **MaybeExecuted**
   - `Free` / `Consumed` while still pending → **MaybeExecuted** (protocol mismatch)

Framework still fails all pending futures, but app receives the classification.

### 3) Failure Callback Extension
Extend failure info so the app can decide:
- `last_runtime_state` (from shm snapshot)
- `replay_hint` (SafeToReplay / MaybeExecuted)

No default auto-replay in framework; app decides per request.

### 4) Session Lifecycle
- Crash → session invalid, client fails pending, application decides replay.
- Restart uses new `OpenSession()` (new shm/session_id allowed).

---

## Idle Auto-Exit (Framework Level)

**Why framework-level:** It controls session lifetime, shm/eventfd ownership, and interacts with crash/recovery.

### Config (server-side)
Add to server config:
- `idle_timeout_ms` (0 = disabled)
- `idle_check_interval_ms` (default 500–1000ms)
- `exit_on_close_session` (true by default)

### Behavior
- Maintain `inflight_count` and `last_activity_mono_ms`.
- When `inflight_count == 0` and `now - last_activity >= idle_timeout_ms`, server transitions to `Closing` and exits cleanly.
- `CloseSession()` sets `session_state = Closing` to request an early exit.

### Semantics
- Idle exit is best-effort; app can disable or override by keeping `idle_timeout_ms = 0`.
- Exit decision is independent from crash handling.

---

## Compatibility And Migration
- No API changes for application RPC calls.
- Failure callback adds fields; existing callbacks remain valid (zero/default values when unused).
- POSIX demo bootstrap continues to create new shm on each `OpenSession()`.

---

## Testing Strategy
- Unit: crash classification for each runtime state.
- Integration: crash mid-queue vs mid-exec; verify SafeToReplay vs MaybeExecuted.
- Idle exit: no requests for X seconds triggers shutdown; `CloseSession()` triggers shutdown.

---

## Alternatives Considered
- Dedicated “start-exec ack” channel: increases complexity and still lossy on crash.
- No classification (all maybe-executed): too pessimistic for your replay needs.

---

## Open Questions
- Do we need to expose `idle_exit_reason` to app diagnostics?
- Should we allow per-opcode default replay policy (e.g., via registry)?

