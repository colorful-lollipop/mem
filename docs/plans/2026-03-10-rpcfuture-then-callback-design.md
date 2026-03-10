# RpcFuture::Then() Callback Design

## Problem

Every `RpcFuture::Wait()` blocks on a per-future `std::mutex` + `std::condition_variable`. While the per-future lock avoids contention between waiters, each completion triggers a kernel futex wakeup syscall. For pure-async callers who don't need to block a thread, this is unnecessary overhead.

## Design

Add a `Then(callback)` method to `RpcFuture`. When a callback is registered, the dispatcher thread invokes it directly upon response arrival — no mutex acquire, no CV notify, no kernel wakeup on the consumer side.

### API

```cpp
// Register a completion callback. Executed on the dispatcher thread.
// If the future is already ready, callback runs immediately on the calling thread.
// Mutually exclusive with Wait/WaitFor/WaitAndTake on the same future.
void RpcFuture::Then(std::function<void(RpcReply)> callback);
```

Typed convenience in `typed_invoker.h`:

```cpp
template <typename Rep>
void ThenDecode(RpcFuture future, std::function<void(StatusCode, Rep)> callback);
```

### State Change

```cpp
struct RpcFuture::State {
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool abandoned = false;
  RpcReply reply;
  std::function<void(RpcReply)> callback;  // NEW
};
```

### Completion Path

In both `CompleteRequest()` and `ResolveFuture()`, after setting `ready = true`:

```
if callback is set:
    move callback out
    unlock mutex            // release lock BEFORE invoking callback
    invoke callback(reply)
else:
    cv.notify_one()         // existing Wait path unchanged
```

Unlocking before callback invocation prevents deadlock if the callback touches the same RpcClient.

### Then() Implementation

```
lock mutex
if already ready:
    unlock
    invoke callback(move(reply))   // immediate execution
    return
store callback
unlock
```

### Constraints

1. **Callback must not block** — runs on the dispatcher thread; blocking stalls all response delivery.
2. **Then() and Wait() are mutually exclusive** — calling both on the same future is a usage error. Then() takes priority; a subsequent Wait() returns `InvalidArgument`.
3. **Already-ready futures** — `Then()` executes the callback synchronously on the calling thread when the future was pre-resolved (e.g., fast-fail `InvalidArgument`).

## Files to Change

| File | Change |
|------|--------|
| `include/memrpc/client/rpc_client.h` | Add `Then()` declaration |
| `src/client/rpc_client.cpp` | Add `callback` to State; implement `Then()`; branch in `CompleteRequest()` and `ResolveFuture()` |
| `include/memrpc/client/typed_invoker.h` | Add `ThenDecode<T>()` template |
| Tests (new) | Then callback, already-ready Then, error callback, Then+Wait mutual exclusion |

## Restoration Note (2026-03-10)

For the immediate restoration, we will **match the previously implemented behavior**:
- No explicit enforcement of Then/Wait mutual exclusion (usage is documented but not guarded).
- When a callback is present, the completion path invokes it and **does not** notify waiters.
- Already-ready futures invoke the callback synchronously on the calling thread.

This keeps behavior consistent with the prior implementation and avoids extra changes beyond restoring the missing logic.

Existing callers using Wait/WaitFor/WaitAndTake require **zero changes**.
