# RpcClient Decomposition And AnyCall Boundary Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Split `memrpc::RpcClient` into explicit internal components and shrink VES `AnyCall` back to a strict large-payload fallback so lifecycle ownership stays singular and the client/server transport model becomes readable again.

**Architecture:** Keep the public `memrpc/client/rpc_client.h` API stable in the first phase, but internally separate session transport, recovery state, request tracking, submission, response dispatch, and watchdog concerns. On the VES side, replace the current "same handlers, two transport paths" model with an explicit fallback dispatcher that only serves whitelisted large-payload operations and never owns lifecycle or recovery semantics.

**Tech Stack:** C++17, memrpc shared-memory transport, OHOS SA mock, GoogleTest, CMake/CTest

---

## 1. Problem Statement

This plan addresses two codebase problems together because they amplify each other:

1. `RpcClient::Impl` is a monolith.
   It currently mixes transport attachment, lifecycle state, request bookkeeping, worker-thread control, timeout scanning, health checks, engine-death handling, and public API gating in one implementation unit.

2. `AnyCall` has expanded from "transport downgrade" into a second semi-parallel RPC path.
   The same VES business handlers are registered into both `RpcServer` and a side `anyCallHandlers_` map, which forces maintainers to reason about two transport models for one API surface.

These problems share one root cause:

- ownership boundaries are documented, but not enforced structurally
- lifecycle and transport concerns leak across layers
- "fallback" code paths are too generic, so they start behaving like main paths

## 2. Design Goals

- Keep `RpcClient` as the only lifecycle owner.
- Keep `VesClient` as a VES-specific adapter, not a second recovery machine.
- Keep `AnyCall` as a transport downgrade only.
- Preserve current external behavior where possible during the refactor.
- Make the internal design readable from file boundaries, not only from comments.
- Allow focused tests for recovery, request tracking, and fallback selection without starting the full stack.

## 3. Non-Goals

- Do not redesign the shared-memory wire protocol.
- Do not replace `AnyCall` with a general streaming or chunking protocol in this plan.
- Do not move typed business codecs into `memrpc`.
- Do not change VES business semantics for `ScanFile()` beyond clarifying transport selection and recovery ownership.

## 4. Target Architecture

### 4.1 `memrpc` target shape

Public API remains:

- `memrpc/include/memrpc/client/rpc_client.h`

Internal implementation is split into these private components under `memrpc/src/client/`:

- `client_session_transport.{h,cpp}`
  Owns `IBootstrapChannel`, `Session`, session snapshot publication, open/close, and engine-death callback wiring.
- `client_recovery_state.{h,cpp}`
  Owns lifecycle state, recovery window state, structured snapshots, structured recovery events, and recovery waiters.
- `client_request_store.{h,cpp}`
  Owns queued submissions, admitted pending requests, deadlines, and bulk-fail/drain helpers.
- `client_submit_pump.{h,cpp}`
  Owns admission loop, request publication, request-credit waits, and submission retry semantics.
- `client_response_pump.{h,cpp}`
  Owns response-ring draining, late-reply handling, event delivery, and response-poll failure detection.
- `client_watchdog.{h,cpp}`
  Owns pending timeout scanning, idle-policy evaluation, and health-check scheduling.
- `rpc_client.cpp`
  Becomes an orchestration facade that wires the components together and implements the public methods.

First migration phase may keep some of these as file-local classes in `rpc_client.cpp`.
The design target is still the same: one class per responsibility domain.

### 4.2 `virus_executor_service` target shape

The VES client path is split conceptually into:

- `VesClient`
  Encodes typed requests, chooses inline memrpc vs large-payload fallback, applies default VES recovery policy, and delegates recovery waiting to `RpcClient`.
- `VesBootstrapChannel`
  Only manages control binding, `OpenSession`, `CloseSession`, `CheckHealth`, and engine-death callback forwarding.
- `VesFallbackInvoker`
  A thin helper used only for large-payload fallback operations; it does not cache lifecycle state.
- `VesAnyCallDispatcher`
  A server-side explicit dispatcher for the small set of operations allowed to use fallback transport.

### 4.3 Ownership rules

The new hard boundaries are:

- `RpcClient`
  owns lifecycle transitions
  owns recovery windows
  owns pending request failure semantics
  owns engine-death and health-triggered recovery

- `VesClient`
  owns typed API encoding and payload-size-based route selection
  does not own cooldown state
  does not infer transport health on its own

- `VesBootstrapChannel`
  owns only control acquisition and control invalidation
  does not pre-open sessions after death
  does not perform business retries

- `AnyCall`
  may carry VES request/response payloads
  may not define session lifecycle
  may not define recovery transitions
  may not mirror framework transport state

## 5. Detailed `RpcClient` Design

### 5.1 Public surface

Keep these public methods stable:

- `Init()`
- `InvokeAsync()`
- `RetryUntilRecoverySettles()`
- `GetRuntimeStats()`
- `GetRecoveryRuntimeSnapshot()`
- `Shutdown()`

Add only one optional public helper if needed by fallback orchestration:

```cpp
StatusCode WaitForRecoveryObservationChange(uint64_t previousVersion,
                                            std::chrono::steady_clock::time_point deadline);
```

This helper is framework-owned and should be preferred over VES-side condition variables if a fallback call must wait for a framework lifecycle change.

### 5.2 Internal component contracts

#### `ClientSessionTransport`

Responsibilities:

- bind/rebind `IBootstrapChannel`
- install/clear engine-death callback
- open live session
- close live session
- publish immutable `SessionSnapshot`
- expose locked access to `Session`

Non-responsibilities:

- no recovery policy
- no pending-request failure decisions
- no timeout scanning

Suggested interface:

```cpp
class ClientSessionTransport {
public:
    void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap,
                             std::function<void(uint64_t)> deathCallback);
    StatusCode OpenSession();
    uint64_t CloseLiveSession();
    bool HasLiveSession() const;
    uint64_t CurrentSessionId() const;
    std::shared_ptr<const SessionSnapshot> LoadSnapshot() const;

    template <typename Fn>
    auto WithSessionLocked(Fn&& fn) -> decltype(fn(std::declval<Session&>()));
};
```

#### `ClientRecoveryState`

Responsibilities:

- own `ClientLifecycleState`
- own `RecoveryTrigger` and `RecoveryAction` history
- own cooldown window timing
- own structured events and snapshots
- own wakeups for recovery waiters
- own observation version increments

Non-responsibilities:

- no session attach/detach
- no polling
- no request mutation

Important semantic rule:

- every lifecycle-visible change increments an observation counter
- waiting code should wait on observation change, not on ad-hoc booleans

#### `ClientRequestStore`

Responsibilities:

- own queued submission objects
- own admitted pending requests
- own deadline eviction
- own bulk drain and fail lists

Non-responsibilities:

- no transport access
- no policy decisions

This splits "where the requests are" from "how they move".

#### `ClientSubmitPump`

Responsibilities:

- pop queued submissions
- ensure live session exists
- wait through cooldown or recovery windows via `ClientRecoveryState`
- publish requests into the request ring
- handle request-credit waits
- register admitted requests in `ClientRequestStore`

Failure model:

- admission-stage failures are reported here
- policy lookup is delegated back to the orchestrator

#### `ClientResponsePump`

Responsibilities:

- poll response event fd
- drain response ring
- match replies to pending requests
- ignore late replies cleanly
- deliver framework events
- surface poll failure as engine-death detection

#### `ClientWatchdog`

Responsibilities:

- scan expired pending requests
- run periodic health checks
- run idle policy checks

Non-responsibilities:

- no direct session open/close logic
- no direct failure resolution beyond callbacks into the orchestrator

### 5.3 Orchestrator rules

`RpcClient::Impl` becomes a coordinator with these duties only:

- compose the private components
- translate component callbacks into policy decisions
- keep API lifecycle state separate from recovery lifecycle state
- start/stop worker threads
- expose public methods

The orchestrator may still own:

- `RecoveryPolicy`
- public callbacks
- worker thread handles
- top-level shutdown sequencing

It should not own:

- request containers
- detailed recovery timers
- detailed session mechanics

### 5.4 State model

Two distinct state domains remain:

- API lifecycle:
  `Open -> Closing -> Closed`
- recovery lifecycle:
  `Uninitialized -> Active -> Cooldown/Recovering/Disconnected/IdleClosed -> Active`

These must never be collapsed.

Reason:

- `Closed` is a terminal API gate
- `Disconnected` and `IdleClosed` are recoverable runtime states

### 5.5 Recovery decision flow

All recovery decisions pass through one orchestrator method:

```cpp
void ApplyRecoveryDecision(const RecoveryDecision& decision,
                           RecoveryTrigger trigger,
                           RecoverySource source);
```

Where `RecoverySource` is internal-only:

```cpp
enum class RecoverySource : uint8_t {
    FailurePolicy,
    EngineDeath,
    ExternalHealthSignal,
    IdlePolicy,
};
```

This removes hidden semantic differences between timeout-triggered recovery and engine-death-triggered recovery.

The only intentional difference that remains:

- engine death fails in-flight requests as `CrashedDuringExecution`
- non-engine-death restart requests fail them as `PeerDisconnected`

That difference should be encoded in one place, not scattered across workers.

## 6. Detailed `AnyCall` Boundary Design

### 6.1 Allowed use

`AnyCall` is allowed only when:

- the typed API is explicitly marked fallback-capable
- the encoded request payload exceeds `memrpc` inline request capacity

It is not allowed because:

- memrpc session is temporarily unhealthy
- a caller wants a "backup transport"
- recovery is in progress

If memrpc is in recovery, `VesClient` may wait using `RpcClient` recovery primitives and retry the same route selection.
It may not silently switch a memrpc-sized request to `AnyCall`.

### 6.2 Client-side shape

Add a private helper:

```cpp
class VesFallbackInvoker {
public:
    template <typename Reply>
    MemRpc::StatusCode Invoke(OHOS::sptr<IVirusProtectionExecutor> control,
                              MemRpc::Opcode opcode,
                              MemRpc::Priority priority,
                              uint32_t execTimeoutMs,
                              std::vector<uint8_t> payload,
                              Reply* reply);
};
```

`VesClient::InvokeApi()` becomes:

1. encode typed request
2. classify route
3. if inline-capable, call memrpc path
4. if large-payload and fallback-capable, call `VesFallbackInvoker`
5. wrap the whole route selection in `RpcClient::RetryUntilRecoverySettles()`

Important rule:

- recovery waiting happens around route selection
- fallback invocation itself does not maintain separate recovery state

### 6.3 Operation registry

Add an explicit fallback capability table in VES client code:

```cpp
enum class VesTransportRoute : uint8_t {
    MemRpcOnly,
    MemRpcOrLargePayloadFallback,
};

VesTransportRoute RouteForOpcode(MemRpc::Opcode opcode);
```

Initially:

- `ScanFile` => `MemRpcOrLargePayloadFallback`

All new typed APIs must opt in explicitly.

### 6.4 Server-side shape

Replace the generic side `anyCallHandlers_` mirror with an explicit dispatcher:

```cpp
class VesAnyCallDispatcher {
public:
    void RegisterTypedFallback(MemRpc::Opcode opcode, MemRpc::RpcHandler handler);
    MemRpc::StatusCode Dispatch(const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) const;
};
```

Key change:

- `RpcHandlerRegistrar` continues to register memrpc handlers to `RpcServer`
- fallback registration becomes a separate explicit action

Do not reuse the same generic `RegisterHandlers()` hook for both transports.

Reason:

- one hook encourages "everything is fallback-capable"
- explicit fallback registration forces per-operation transport decisions

### 6.5 `EngineSessionService` changes

`EngineSessionService` should keep only:

- bootstrap/session service ownership
- `RpcServer` ownership
- optional event publisher
- `VesAnyCallDispatcher` ownership

It should stop acting as a transport-agnostic dual registration hub.

Target constructor shape:

```cpp
EngineSessionService(std::vector<RpcHandlerRegistrar*> memrpcRegistrars,
                     std::vector<VesAnyCallRegistrar*> anyCallRegistrars);
```

If that is too heavy for phase 1, keep one constructor but separate the registration calls internally:

- `RegisterMemRpcHandlers()`
- `RegisterAnyCallFallbackHandlers()`

### 6.6 Handler registration split

Add a second registration interface:

```cpp
class VesAnyCallRegistrar {
public:
    virtual ~VesAnyCallRegistrar() = default;
    virtual void RegisterAnyCallHandlers(VesAnyCallDispatcher* dispatcher) = 0;
};
```

Business services may implement both interfaces, but the code must say so explicitly.

That makes the transport decision visible at compile time.

## 7. File-Level Refactor Plan

### 7.1 `memrpc`

Create or extract:

- `memrpc/src/client/client_session_transport.h`
- `memrpc/src/client/client_session_transport.cpp`
- `memrpc/src/client/client_recovery_state.h`
- `memrpc/src/client/client_recovery_state.cpp`
- `memrpc/src/client/client_request_store.h`
- `memrpc/src/client/client_request_store.cpp`
- `memrpc/src/client/client_submit_pump.h`
- `memrpc/src/client/client_submit_pump.cpp`
- `memrpc/src/client/client_response_pump.h`
- `memrpc/src/client/client_response_pump.cpp`
- `memrpc/src/client/client_watchdog.h`
- `memrpc/src/client/client_watchdog.cpp`

Modify:

- `memrpc/src/client/rpc_client.cpp`
- `memrpc/src/CMakeLists.txt`

Keep untouched initially:

- `memrpc/include/memrpc/client/rpc_client.h`
- `memrpc/src/core/session.cpp`
- `memrpc/src/server/rpc_server.cpp`

### 7.2 `virus_executor_service`

Create:

- `virus_executor_service/include/transport/ves_anycall_dispatcher.h`
- `virus_executor_service/src/transport/ves_anycall_dispatcher.cpp`
- `virus_executor_service/include/service/ves_anycall_registrar.h`

Modify:

- `virus_executor_service/include/client/ves_client.h`
- `virus_executor_service/src/client/ves_client.cpp`
- `virus_executor_service/include/ves/ves_session_service.h`
- `virus_executor_service/src/service/ves_session_service.cpp`
- `virus_executor_service/include/service/rpc_handler_registrar.h`
- `virus_executor_service/src/service/virus_executor_service.cpp`
- `virus_executor_service/CMakeLists.txt`

## 8. Testing Strategy

### 8.1 `RpcClient` split verification

Existing targeted tests to keep green:

- `memrpc_rpc_client_api_test`
- `memrpc_rpc_client_shutdown_race_test`
- `memrpc_engine_death_handler_test`
- `memrpc_rpc_client_external_recovery_test`
- `memrpc_rpc_client_recovery_policy_test`
- `memrpc_rpc_client_timeout_watchdog_test`
- `memrpc_rpc_client_idle_callback_test`

Add focused new tests:

- `client_request_store_test.cpp`
  verifies expiry, take-all, late-reply removal
- `client_recovery_state_test.cpp`
  verifies observation version bumps and lifecycle transitions
- `client_session_transport_test.cpp`
  verifies open/close/death-callback semantics without request logic

### 8.2 `AnyCall` boundary verification

Add or modify VES tests to assert:

- memrpc-sized payloads never route to `AnyCall`
- oversized payloads route to `AnyCall` only for fallback-capable opcodes
- `AnyCall` does not bypass memrpc recovery ownership
- engine death and cooldown are still observed from `RpcClient`
- dual registration no longer happens through one generic sink

Suggested targets:

- `virus_executor_service_testkit_client_test`
- `virus_executor_service_codec_test`
- `virus_executor_service_policy_test`
- new `virus_executor_service_anycall_dispatcher_test`
- new `virus_executor_service_transport_selection_test`

### 8.3 Integration verification

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_api|rpc_client_shutdown_race|engine_death_handler|rpc_client_external_recovery|rpc_client_recovery_policy|rpc_client_timeout_watchdog|rpc_client_idle_callback)'
```

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_(policy|codec|testkit_client|session_service|heartbeat|crash_recovery)'
```

Run:

```bash
tools/push_gate.sh
```

For lifecycle/recovery-sensitive changes, also run:

```bash
tools/push_gate.sh --deep
```

## 9. Risks And Mitigations

- Risk: the refactor changes shutdown timing subtly.
  Mitigation: lock down shutdown race tests before splitting code.

- Risk: internal extraction duplicates mutex ownership or introduces deadlocks.
  Mitigation: each component owns one clear mutex domain and exposes callback-based integration only.

- Risk: fallback registration split breaks current tests that assume all handlers are fallback-capable.
  Mitigation: add an explicit opcode capability table first, then migrate one opcode at a time.

- Risk: moving code into more files makes private helper access harder.
  Mitigation: prefer small internal classes with narrow constructor dependencies instead of shared global state.

## 10. Migration Sequence

### Task 1: Lock down current `RpcClient` behavior before extraction

**Files:**
- Modify: `memrpc/tests/rpc_client_api_test.cpp`
- Modify: `memrpc/tests/rpc_client_shutdown_race_test.cpp`
- Modify: `memrpc/tests/rpc_client_timeout_watchdog_test.cpp`
- Modify: `memrpc/tests/engine_death_handler_test.cpp`

**Step 1: Add focused assertions for current public semantics**

Add tests that pin:

- shutdown is terminal
- cooldown/recovering states still block admission the same way
- engine death still converts in-flight work to `CrashedDuringExecution`
- health-check recovery still routes through framework state

**Step 2: Run tests to verify the baseline**

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_api|rpc_client_shutdown_race|rpc_client_timeout_watchdog|engine_death_handler)'
```

Expected:
- all tests pass before extraction starts

**Step 3: Commit**

```bash
git add memrpc/tests/rpc_client_api_test.cpp memrpc/tests/rpc_client_shutdown_race_test.cpp memrpc/tests/rpc_client_timeout_watchdog_test.cpp memrpc/tests/engine_death_handler_test.cpp
git commit -m "test: lock rpc client lifecycle baseline"
```

### Task 2: Extract recovery state into a standalone internal component

**Files:**
- Create: `memrpc/src/client/client_recovery_state.h`
- Create: `memrpc/src/client/client_recovery_state.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/src/CMakeLists.txt`
- Test: `memrpc/tests/client_recovery_state_test.cpp`

**Step 1: Create `ClientRecoveryState` with lifecycle, cooldown, callbacks, and observation version**

Include:

- lifecycle transition methods
- snapshot/report generation
- recovery wait methods
- observation version tracking

**Step 2: Add focused unit tests for transition and wait semantics**

Test:

- version increments on state-visible changes
- cooldown wait returns `CooldownActive` on deadline expiry
- terminal close blocks later recovery transitions

**Step 3: Rewire `rpc_client.cpp` to call the new component**

Leave behavior unchanged.

**Step 4: Run tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_recovery_policy|rpc_client_external_recovery|rpc_client_idle_callback)|client_recovery_state_test'
```

Expected:
- focused tests pass

**Step 5: Commit**

```bash
git add memrpc/src/client/client_recovery_state.h memrpc/src/client/client_recovery_state.cpp memrpc/src/client/rpc_client.cpp memrpc/src/CMakeLists.txt memrpc/tests/client_recovery_state_test.cpp
git commit -m "refactor: extract rpc client recovery state"
```

### Task 3: Extract session transport and request storage

**Files:**
- Create: `memrpc/src/client/client_session_transport.h`
- Create: `memrpc/src/client/client_session_transport.cpp`
- Create: `memrpc/src/client/client_request_store.h`
- Create: `memrpc/src/client/client_request_store.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/src/CMakeLists.txt`
- Test: `memrpc/tests/client_session_transport_test.cpp`
- Test: `memrpc/tests/client_request_store_test.cpp`

**Step 1: Move session attach/open/close/snapshot logic into `ClientSessionTransport`**

**Step 2: Move queued/admitted request containers into `ClientRequestStore`**

**Step 3: Add focused unit tests**

Test:

- session open publishes expected snapshot
- death callback retention survives bootstrap swaps
- request expiry and bulk drain work without the rest of `RpcClient`

**Step 4: Run tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_api|rpc_client_shutdown_race|rpc_client_timeout_watchdog)|client_(session_transport|request_store)_test'
```

Expected:
- old lifecycle tests still pass
- new component tests pass

**Step 5: Commit**

```bash
git add memrpc/src/client/client_session_transport.h memrpc/src/client/client_session_transport.cpp memrpc/src/client/client_request_store.h memrpc/src/client/client_request_store.cpp memrpc/src/client/rpc_client.cpp memrpc/src/CMakeLists.txt memrpc/tests/client_session_transport_test.cpp memrpc/tests/client_request_store_test.cpp
git commit -m "refactor: extract rpc client transport and request store"
```

### Task 4: Extract submit/response/watchdog pumps

**Files:**
- Create: `memrpc/src/client/client_submit_pump.h`
- Create: `memrpc/src/client/client_submit_pump.cpp`
- Create: `memrpc/src/client/client_response_pump.h`
- Create: `memrpc/src/client/client_response_pump.cpp`
- Create: `memrpc/src/client/client_watchdog.h`
- Create: `memrpc/src/client/client_watchdog.cpp`
- Modify: `memrpc/src/client/rpc_client.cpp`
- Modify: `memrpc/src/CMakeLists.txt`

**Step 1: Move worker loop code without changing behavior**

Keep orchestrator callbacks narrow:

- resolve reply
- apply policy
- notify recovery waiters
- stop/join threads

**Step 2: Re-run the full memrpc recovery subset**

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_api|rpc_client_shutdown_race|engine_death_handler|rpc_client_external_recovery|rpc_client_recovery_policy|rpc_client_timeout_watchdog|rpc_client_idle_callback|rpc_eventfd_fault_injection)'
```

Expected:
- no regression in targeted client behavior

**Step 3: Commit**

```bash
git add memrpc/src/client/client_submit_pump.h memrpc/src/client/client_submit_pump.cpp memrpc/src/client/client_response_pump.h memrpc/src/client/client_response_pump.cpp memrpc/src/client/client_watchdog.h memrpc/src/client/client_watchdog.cpp memrpc/src/client/rpc_client.cpp memrpc/src/CMakeLists.txt
git commit -m "refactor: extract rpc client worker pumps"
```

### Task 5: Introduce explicit `AnyCall` capability routing in `VesClient`

**Files:**
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`
- Test: `virus_executor_service/tests/unit/testkit/testkit_client_test.cpp`
- Create: `virus_executor_service/tests/unit/ves/ves_transport_selection_test.cpp`

**Step 1: Add a route classifier and private `VesFallbackInvoker`**

Classify each opcode as:

- memrpc-only
- memrpc-or-large-payload-fallback

**Step 2: Keep recovery waiting framework-owned**

Route selection remains inside `client_.RetryUntilRecoverySettles(...)`.
Do not add new VES-side recovery condition variables.

**Step 3: Add tests**

Test:

- small `ScanFile` payload uses memrpc path
- oversized `ScanFile` payload uses fallback path
- recovery still waits on `RpcClient` and does not switch route just because memrpc is unhealthy

**Step 4: Run tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_(policy|testkit_client)|ves_transport_selection_test'
```

Expected:
- VES client route selection is deterministic

**Step 5: Commit**

```bash
git add virus_executor_service/include/client/ves_client.h virus_executor_service/src/client/ves_client.cpp virus_executor_service/tests/unit/ves/ves_policy_test.cpp virus_executor_service/tests/unit/testkit/testkit_client_test.cpp virus_executor_service/tests/unit/ves/ves_transport_selection_test.cpp
git commit -m "refactor: make ves transport selection explicit"
```

### Task 6: Replace generic mirrored `AnyCall` handler registration

**Files:**
- Create: `virus_executor_service/include/transport/ves_anycall_dispatcher.h`
- Create: `virus_executor_service/src/transport/ves_anycall_dispatcher.cpp`
- Create: `virus_executor_service/include/service/ves_anycall_registrar.h`
- Modify: `virus_executor_service/include/service/rpc_handler_registrar.h`
- Modify: `virus_executor_service/include/ves/ves_session_service.h`
- Modify: `virus_executor_service/src/service/ves_session_service.cpp`
- Modify: `virus_executor_service/src/service/virus_executor_service.cpp`
- Modify: `virus_executor_service/CMakeLists.txt`
- Create: `virus_executor_service/tests/unit/ves/ves_anycall_dispatcher_test.cpp`

**Step 1: Add explicit fallback dispatcher and registrar interfaces**

Stop using one generic sink to mirror all handlers into fallback transport.

**Step 2: Register only fallback-capable operations**

Start with `ScanFile` only.

**Step 3: Update `EngineSessionService` to compose `RpcServer` and `VesAnyCallDispatcher` separately**

**Step 4: Add dispatcher tests**

Test:

- unregistered opcode returns `InvalidArgument`
- registered fallback opcode dispatches correctly
- memrpc handler registration and fallback registration are independent

**Step 5: Run tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'virus_executor_service_(session_service|codec|policy)|ves_anycall_dispatcher_test'
```

Expected:
- explicit fallback path works
- generic mirroring is gone

**Step 6: Commit**

```bash
git add virus_executor_service/include/transport/ves_anycall_dispatcher.h virus_executor_service/src/transport/ves_anycall_dispatcher.cpp virus_executor_service/include/service/ves_anycall_registrar.h virus_executor_service/include/service/rpc_handler_registrar.h virus_executor_service/include/ves/ves_session_service.h virus_executor_service/src/service/ves_session_service.cpp virus_executor_service/src/service/virus_executor_service.cpp virus_executor_service/CMakeLists.txt virus_executor_service/tests/unit/ves/ves_anycall_dispatcher_test.cpp
git commit -m "refactor: isolate ves anycall fallback dispatch"
```

### Task 7: Run integration and push-gate verification

**Files:**
- Test: `memrpc/tests/*`
- Test: `virus_executor_service/tests/*`

**Step 1: Run focused memrpc and VES suites**

Run:

```bash
tools/build_and_test.sh --test-regex 'memrpc_(rpc_client_api|rpc_client_shutdown_race|engine_death_handler|rpc_client_external_recovery|rpc_client_recovery_policy|rpc_client_timeout_watchdog|rpc_client_idle_callback)|virus_executor_service_(policy|codec|session_service|heartbeat|crash_recovery)'
```

Expected:
- focused suites pass

**Step 2: Run push gate**

Run:

```bash
tools/push_gate.sh
```

Expected:
- push gate passes

**Step 3: Run deep gate for lifecycle-sensitive changes**

Run:

```bash
tools/push_gate.sh --deep
```

Expected:
- deep gate passes without transport/recovery regressions

**Step 4: Commit final integration fixes if needed**

```bash
git add <touched files>
git commit -m "test: validate rpc client split and anycall boundary"
```
