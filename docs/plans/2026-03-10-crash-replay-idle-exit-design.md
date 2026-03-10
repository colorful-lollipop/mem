# Crash Replay Classification And Idle Exit Design

**Goal:** After server crash, classify each pending request as safe-to-replay vs maybe-executed (“poison”), add client-side async timeout detection, and provide client-side idle reminders so applications can decide when to close sessions.

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
- Provide async timeout detection in the client using existing timeout fields.
- Provide idle detection in the client as a reminder callback (no automatic exit).
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
- `stage = Timeout` for async timeout watchdog events

No default auto-replay in framework; app decides per request.

### 4) Async Timeout Watchdog (Client)
Client adds a lightweight watchdog thread to cover server-side timeouts for async calls:
- **Admission** timeout is still enforced by the submitter (waiting for slot/ring).
- Watchdog covers **server phase** only, using:
  - `queue_timeout_ms` with `enqueue_mono_ms`
  - `exec_timeout_ms` with `start_exec_mono_ms` (fallback to `enqueue_mono_ms` if unset)
- When timeout fires:
  - Emit `RpcFailureCallback` with `FailureStage::Timeout`
  - Use runtime state to pick `StatusCode::QueueTimeout` vs `StatusCode::ExecTimeout`
  - Release request slot and clear pending state
  - Late responses are discarded via `request_id` mismatch checks (response slot still released)

### 5) Session Lifecycle
- Crash → session invalid, client fails pending, application decides replay.
- Restart uses new `OpenSession()` (new shm/session_id allowed).

---

## Idle Reminder (Client Level)

**Why client-level:** Only the client can decide whether to close a session; server cannot know app policy.

### Config (client-side)
Idle timeout and notify interval are **compile-time constants** (default 0 = disabled).

### Behavior
- Client watchdog tracks last activity (submit, reply, or session attach).
- When idle time exceeds threshold, fire `IdleCallback` periodically.
- Application decides to call `CloseSession()` when appropriate.

### Semantics
- Idle reminder is best-effort; app can disable by compiling with timeout = 0.
- Reminder does not change session state automatically.

---

## Compatibility And Migration
- No API changes for application RPC calls.
- Failure callback adds fields; existing callbacks remain valid (zero/default values when unused).
- POSIX demo bootstrap continues to create new shm on each `OpenSession()`.

---

## Testing Strategy
- Unit: crash classification for each runtime state.
- Integration: crash mid-queue vs mid-exec; verify SafeToReplay vs MaybeExecuted.
- Async timeout: verify watchdog triggers QueueTimeout/ExecTimeout with FailureStage::Timeout.
- Idle reminder: no requests for X seconds triggers callback; callback repeats per interval.

---

## Alternatives Considered
- Dedicated “start-exec ack” channel: increases complexity and still lossy on crash.
- No classification (all maybe-executed): too pessimistic for your replay needs.

---

## Open Questions
- Do we need to expose `idle_exit_reason` to app diagnostics?
- Should we allow per-opcode default replay policy (e.g., via registry)?
