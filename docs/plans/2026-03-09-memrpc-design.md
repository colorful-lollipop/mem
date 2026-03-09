# Shared-Memory Engine RPC Design

## Goal

Build a two-process function-call framework over Linux shared memory for an antivirus engine split, with:

- C++17 implementation
- CMake-based development and verification
- GN build translation at the end
- high performance and stable failure handling
- an OO-style synchronous compatibility layer for existing business code
- support for one client process issuing concurrent scan requests
- two request priorities, with the engine process using separate worker pools and allowing normal work to be starved by high-priority work
- bootstrap abstraction so HarmonyOS SA can be integrated without polluting the transport core

## Non-Goals

- Multi-client shared access in v1
- Generic RPC for arbitrary object graphs in v1
- Forced cancellation/preemption of in-flight scan tasks
- Binding directly to a concrete HarmonyOS SA implementation in the demo

## Recommended Architecture

Use a fixed shared-memory region with:

- one high-priority request ring
- one normal-priority request ring
- one response ring
- one shared slot pool for request/response payloads
- eventfd-based notification
- a bootstrap abstraction to exchange file descriptors and session metadata

The public API remains synchronous for callers. Internally, each call is turned into an asynchronous transaction keyed by `request_id`.

## Layering

### `memrpc_core`

Owns protocol definitions and runtime primitives:

- shared memory layout
- ring buffers
- slot pool
- request identifiers
- timeout helpers
- session state

### `memrpc_client`

Owns client-side request lifecycle:

- `EngineClient`
- pending request table
- result dispatcher thread
- synchronous wait wrapper for legacy call sites

### `memrpc_server`

Owns engine-side execution:

- queue draining
- dispatch to priority-specific worker pools
- invoke business scan handler
- write responses back to shared memory

### `memrpc_bootstrap`

Defines bootstrap interfaces independent of transport core:

- start/connect session
- exchange shared memory and eventfd handles
- report peer/session replacement

### `memrpc_bootstrap_posix_demo`

Provides a Linux-only demo bootstrap so the framework can be built and tested outside HarmonyOS.

### `memrpc_bootstrap_sa_stub`

Defines the HarmonyOS SA adapter surface and integration points without requiring the real SA runtime in development.

## Public API

### Client-facing API

```cpp
namespace memrpc {

enum class Priority {
  Normal = 0,
  High = 1,
};

struct ScanOptions {
  Priority priority = Priority::Normal;
  uint32_t queue_timeout_ms = 1000;
  uint32_t exec_timeout_ms = 30000;
  uint32_t flags = 0;
};

struct ScanRequest {
  std::string file_path;
  ScanOptions options;
};

enum class ScanVerdict {
  Clean = 0,
  Infected = 1,
  Unknown = 2,
  Error = 3,
};

enum class StatusCode {
  Ok = 0,
  QueueFull,
  QueueTimeout,
  ExecTimeout,
  PeerDisconnected,
  ProtocolMismatch,
  EngineInternalError,
  InvalidArgument,
};

struct ScanResult {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::string message;
};

class EngineClient {
 public:
  StatusCode Init();
  StatusCode Scan(const ScanRequest& request, ScanResult* result);
  void Shutdown();
};

}  // namespace memrpc
```

### Server-facing API

```cpp
namespace memrpc {

class IScanHandler {
 public:
  virtual ~IScanHandler() = default;
  virtual ScanResult HandleScan(const ScanRequest& request) = 0;
};

class EngineServer {
 public:
  StatusCode Start();
  void Run();
  void Stop();
};

}  // namespace memrpc
```

### Bootstrap API

```cpp
namespace memrpc {

struct BootstrapHandles {
  int shm_fd = -1;
  int high_req_event_fd = -1;
  int normal_req_event_fd = -1;
  int resp_event_fd = -1;
  uint32_t protocol_version = 0;
  uint64_t session_id = 0;
};

class IBootstrapChannel {
 public:
  virtual ~IBootstrapChannel() = default;
  virtual StatusCode StartEngine() = 0;
  virtual StatusCode Connect(BootstrapHandles* handles) = 0;
  virtual StatusCode NotifyPeerRestarted() = 0;
};

}  // namespace memrpc
```

## Shared Memory Layout

Use a single shared-memory object for one client-engine session:

```text
+-----------------------------+
| SharedMemoryHeader          |
+-----------------------------+
| HighPriorityRequestRing     |
+-----------------------------+
| NormalPriorityRequestRing   |
+-----------------------------+
| ResponseRing                |
+-----------------------------+
| RequestSlotPool             |
+-----------------------------+
```

### Shared Memory Header

Contains:

- `magic`
- `protocol_version`
- `session_id`
- `state`
- ring capacities
- slot count and slot size
- atomics and statistics
- heartbeat timestamps
- reserved bytes for layout growth

Purpose:

- validate layout and version at connect time
- reject mismatched peers quickly
- support reconnect/session replacement decisions

### Request Rings

Each ring entry is small and only references a slot:

- `request_id`
- `slot_index`
- `opcode`
- `flags`
- `deadline_ms`
- `payload_size`
- `reserved`

There are two independent request rings:

- high-priority ring
- normal-priority ring

### Response Ring

Each response entry includes:

- `request_id`
- `slot_index`
- `status_code`
- `result_size`
- `engine_errno`
- `flags`

### Slot Pool

Each slot stores the request header/payload and response header/payload.

Request payload v1:

- trace/request metadata
- scan flags
- queue timeout
- exec timeout
- file path bytes

Response payload v1:

- status
- verdict
- engine/detail codes
- message bytes

Slot lifecycle:

- `FREE`
- `RESERVED`
- `QUEUED_HIGH` or `QUEUED_NORMAL`
- `DISPATCHED`
- `PROCESSING`
- `RESPONDED`
- `RECLAIMED`

## Synchronization Model

Use three `eventfd` objects:

- `high_req_eventfd`
- `normal_req_eventfd`
- `resp_eventfd`

Client behavior:

- write request into a slot
- push a ring entry into the appropriate request ring
- signal the corresponding request eventfd

Server behavior:

- wait on request eventfds
- drain ready ring entries in batches
- route tasks to the matching worker pool

Response behavior:

- worker writes result into the slot
- worker pushes response entry into response ring
- worker signals `resp_eventfd`

The client result dispatcher drains response entries in batches and wakes the corresponding blocked request waiters.

## Concurrency Model

### Client Side

- one API process
- many caller threads can concurrently call `Scan()`
- one dispatcher thread processes responses
- pending requests are indexed by `request_id`

### Server Side

- one dispatcher thread drains request rings
- one high-priority thread pool consumes only high-priority work
- one normal-priority thread pool consumes only normal-priority work
- high-priority work may starve normal work by design

No attempt is made to preempt running scans.

## Failure and Timeout Semantics

### Timeout Classes

- `queue_timeout_ms`
  Covers slot reservation, queueing, and waiting for server dispatch.
- `exec_timeout_ms`
  Covers scan execution after the server has begun handling the request.

Return codes distinguish:

- `QueueFull`
- `QueueTimeout`
- `ExecTimeout`
- `PeerDisconnected`
- `ProtocolMismatch`
- `EngineInternalError`
- `InvalidArgument`

### Recovery Policy

The framework may reconnect the transport session, but it must not freely replay business requests.

Allowed transparent retry:

- at most one retry
- only when the request can be proven not to have been dispatched to the engine

Disallowed transparent retry:

- after the request reached `DISPATCHED`
- after the request reached `PROCESSING`
- after an execution timeout

When a peer dies after dispatch, return a transport error and let upper layers decide whether to restart the engine process and whether to resubmit the scan.

## HarmonyOS SA Integration

The framework core does not depend on SA.

The SA adapter is responsible for:

- process discovery and startup
- service registration/lookup
- shared memory fd transfer
- eventfd transfer
- version/session metadata exchange
- notifying the client when the engine process has been replaced

Expected integration surface:

- implement `IBootstrapChannel`
- translate SA service calls into `BootstrapHandles`
- keep session identity stable enough for reconnect checks

## Demo Plan

Deliver a Linux demo using a POSIX bootstrap:

- `demo_engine_main`
- `demo_client_main`
- fake scan handler that returns clean/infected/slow/error outcomes
- examples showing normal priority, high priority, timeout, and peer death behavior

## Documentation Plan

Provide:

- architecture doc
- demo guide
- porting guide from old in-process OO API
- SA integration guide

## Build Plan

Develop and verify with CMake first.

At the end, provide equivalent GN files:

- `BUILD.gn`
- `memrpc.gni`

GN targets should cover:

- core library
- client library
- server library
- demo binaries
- tests

## Implementation Constraints

- C++17 only
- prefer fixed-size/shared-memory-friendly structures in the protocol layer
- keep bootstrap transport-specific code isolated
- optimize for stable behavior over maximal feature breadth
- keep the first version narrowly focused on scan-file requests and results
