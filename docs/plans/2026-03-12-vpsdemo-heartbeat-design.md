# vpsdemo SA Heartbeat Design

**Date:** 2026-03-12

## Goal
Add a lightweight heartbeat on the SA socket so the client must call it every 10s after `OpenSession` and treat missing/unhealthy replies as a server failure. On failure the client calls `CloseSession` to trigger SA self-unload and then re-opens the session.

## Scope
- Add a new SA socket command for heartbeat with request-response semantics.
- Reuse existing client and server threads where possible (no new long-lived threads).
- Provide a simple health snapshot in the heartbeat reply (status + current task), with room for future expansion.
- Client treats no reply within 10s or unhealthy reply as failure and restarts.

## Non-Goals
- No redesign of memrpc shared-memory protocol.
- No multi-client or multi-session support; single client <-> single server only.
- No deep health diagnostics (simple checks only).

## Architecture
- **Transport:** SA socket (MockServiceSocket) command `HEARTBEAT` (cmd=3).
- **Client:** reuse `VpsBootstrapProxy::monitor_thread_` to schedule heartbeats every 10s.
- **Server:** reuse `MockServiceSocket` accept thread to handle heartbeat requests; no new thread.
- **Health:** `VpsDemoService` exposes a small health snapshot (`status`, `current_task`, `in_flight`, `last_task_age_ms`). `EngineSessionService` provides `session_id` for reply validation.

## Protocol
Define a fixed-size reply struct in `vps_bootstrap_interface.h`:
- `uint32_t version` (start at 1)
- `uint32_t status` (OK / UNHEALTHY)
- `uint64_t session_id`
- `uint32_t in_flight`
- `uint32_t last_task_age_ms`
- `char current_task[64]` (null-terminated)
- `uint32_t reserved[4]` (future expansion)

The reply is written into `MockIpcReply::data` and does not send any fds.

## Data Flow
1. Client calls `OpenSession` (existing flow) and starts monitoring.
2. Every 10s, `VpsBootstrapProxy::MonitorSocket()` opens a short-lived connection to the SA socket, sends cmd=3, receives `VesHeartbeatReply`, closes the connection.
3. Server `MockServiceSocket` accepts, dispatches to `VpsBootstrapStub::OnRemoteRequest`, which calls `VirusExecutorService::Heartbeat()` to fill the reply and sends it back.
4. Client validates reply. If timeout (no reply within 10s), size mismatch, `status != OK`, or `session_id` mismatch, it triggers restart (see below).

## Client Failure Handling
- On heartbeat timeout/unhealthy:
  - Call `CloseSession()` to request SA self-unload.
  - Invoke `EngineDeathCallback` to drive the existing `RpcClient` restart flow.
- This preserves existing retry/backoff behavior and keeps restart semantics consistent.

## Server Health Criteria (Simple)
`VirusExecutorService::Heartbeat()` reports:
- `OK` when session is initialized and server is not in a known failure state.
- `UNHEALTHY` when session not initialized or server has no active RPC server.
- `current_task` and `last_task_age_ms` are sampled from `VpsDemoService` (simple counters updated around handler execution).

## Threading
- **Client:** reuse existing `monitor_thread_` (no additional thread).
- **Server:** reuse `MockServiceSocket` accept thread (no additional thread).

## Transport Changes (Mock SA)
- Allow data-only replies without fd passing.
- Allow optional `close_after_reply` to close heartbeat connections immediately after reply.

## Error Handling
- Any send/recv/connect failure or malformed reply -> treat as unhealthy.
- Mismatched `session_id` -> unhealthy (stale connection).

## Testing & Verification
- Add a focused test in `demo/vpsdemo/tests`:
  - Heartbeat returns OK after `OpenSession`.
  - Heartbeat returns UNHEALTHY after `CloseSession`.
  - Client triggers restart when heartbeat times out or returns UNHEALTHY.

## Success Criteria
- Client sends heartbeat every 10s using SA socket.
- Server replies with health snapshot.
- Client restarts server on timeout/unhealthy within 10s.
- No new long-lived threads introduced for heartbeat.
