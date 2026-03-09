# Death-Callback Session Recovery Design

## Goal

Extend `memrpc` so the client can recover from engine process death with:

- death-callback-driven session invalidation
- lazy automatic restart on the next `Scan()`
- automatic session replacement through `Connect()`
- a single transparent retry only for requests that were never published to the old shared-memory session

## Constraints

- business traffic continues to use shared memory plus `eventfd`
- HarmonyOS production startup is handled by `init` and SA APIs, not `fork()`
- Linux development may keep using the fake-SA / `fork()` path
- upper layers still own final restart policy and any re-scan policy for requests that might have been observed by the old engine

## Recovery Model

### Engine death

The bootstrap layer notifies the client that the current engine process died.

On that callback, the client must:

- mark the current `session_id` invalid immediately
- fail all pending requests that belong to that session unless they are still in a purely local pre-publish state
- stop waiting for old responses

This avoids stretching failures out to queue or execution timeouts once the process death is already known.

### Lazy recovery

Recovery is triggered by the next `Scan()` call, not by a background reconnection thread.

The recovery flow is:

1. see that the current session is invalid
2. call `StartEngine()`
3. call `Connect()` until a current set of `BootstrapHandles` is returned
4. attach a new `Session`
5. replace the client's active session metadata

This keeps the runtime simpler and aligns with HarmonyOS SA control flow where the client actively starts or reloads the ability after a death callback.

## Request State Model

Each pending request gets a local publish state:

- `kNew`
- `kQueuedLocal`
- `kRingPushed`
- `kSignaled`
- `kCompleted`
- `kFailed`

Retry policy:

- `kNew` and `kQueuedLocal`: may be retried once after recovery
- `kRingPushed` and beyond: never retried automatically

Rationale:

- once the request entry is visible in the shared-memory ring, the old engine may have observed it
- automatic replay after that point risks duplicate scanning

## Bootstrap API Changes

`IBootstrapChannel` gains death-callback registration:

```cpp
using EngineDeathCallback = std::function<void()>;

virtual void SetEngineDeathCallback(EngineDeathCallback callback) = 0;
```

Expectations:

- fake-SA / Linux bootstrap can simulate this callback
- HarmonyOS SA adapter should invoke it when the remote system ability dies

## Client Runtime Changes

`EngineClient` needs:

- a mutex around active session replacement
- a generation/session token on pending calls
- a death callback handler that invalidates the active session and wakes pending waiters
- a helper that ensures there is a live session before publishing a request

Behavior:

- if no valid session exists, `Scan()` performs lazy recovery
- if recovery succeeds before the request is published, the request continues on the new session
- if the death callback arrives after publish, that request completes with `kPeerDisconnected`

## Fake SA Behavior

`SaBootstrapChannel` remains a Linux development adapter but becomes recovery-aware:

- it exposes death notification registration
- it can simulate engine death and session replacement in tests
- internally it may still use the POSIX bootstrap implementation and `fork()`-friendly handles

## Testing

Add tests for:

- death callback invalidates current session and fails old pending work immediately
- next `Scan()` lazily starts a new engine session
- requests not yet published are retried once on the new session
- requests published to the old session are not retried
- `compile_commands.json` export remains enabled in the generated build

## Non-Goals

- background heartbeat polling
- automatic retry for requests that may already be visible to the engine
- automatic bulk replay of old session work
