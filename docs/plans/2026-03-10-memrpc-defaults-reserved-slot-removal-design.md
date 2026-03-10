# MemRpc Defaults And Reserved Slot Removal Design

## Context
The demo bootstrap defaults and shared memory header currently include `high_reserved_request_slots`. This is only used by the client-side `SlotPool` to reserve capacity for high-priority requests. The field adds layout/config complexity and is rarely used. The default ring sizes are also larger than needed for the target concurrency.

## Goals
- Remove `high_reserved_request_slots` from config, layout, and runtime.
- Simplify slot admission logic to ignore priority during slot reservation.
- Update default sizes to a smaller, clearer relationship: `slot_count = 2 * ring_size`.
- Bump the shared memory protocol version to make the layout change explicit.

## Non-Goals
- Preserve wire/layout backward compatibility with existing shared memory segments.
- Change priority-based queue selection (high vs normal ring).
- Modify server worker scheduling or handler behavior.

## Proposed Approach
1. **Shared memory layout change**
   - Remove `high_reserved_request_slots` from `SharedMemoryHeader` and `LayoutConfig`.
   - Update `kProtocolVersion` to `3` so existing shm segments are rejected.

2. **SlotPool simplification**
   - Remove reserved-slot tracking and priority-aware reservation from `SlotPool`.
   - Keep `Priority` usage only for selecting the request ring.

3. **Default sizing update**
   - Defaults become:
     - `high_ring_size = 32`
     - `normal_ring_size = 32`
     - `response_ring_size = 64`
     - `slot_count = 64`

4. **Tests**
   - Remove tests that assert reserved-slot behavior.
   - Update protocol/version expectations where applicable.

## Compatibility And Risk
- This is a deliberate breaking layout change. Existing shared memory segments created with protocol version 2 will be rejected on attach, requiring a restart/recreation.
- Behavior change: high-priority requests no longer have guaranteed reserved slots. High priority still maps to the high-priority request ring.

## Validation
- Run the current unit/integration tests.
- Focus on request/response ring and backpressure tests to ensure behavior remains stable with smaller defaults.
