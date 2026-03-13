# Enum Size Cleanup And Protocol V4 Plan

## Goal

Split the work into a short-term cleanup that has immediate value and a later
protocol v4 change that is only worth doing as a real shared-memory layout
revision.

## Phase 1: Short-Term Enum Cleanup

### Scope

- Change these enums to explicit `uint8_t` underlying types:
  - `memrpc/include/memrpc/core/types.h`
    - `Priority`
    - `ScanVerdict`
    - `StatusCode`
    - `ReplayHint`
    - `RpcRuntimeState`
  - `memrpc/src/core/session.h`
    - `QueueKind`
    - `Session::AttachRole`
  - `memrpc/include/memrpc/client/rpc_client.h`
    - `FailureStage`

### Why This Phase Is Worth Doing

- It removes `NOLINT(performance-enum-size)` from enums that are not bound to a
  fixed shared-memory field width.
- It keeps enum storage aligned with the actual value range.
- It reduces `RpcFailure` from 64 bytes to 48 bytes because
  `StatusCode`/`Priority`/`ReplayHint`/`RpcRuntimeState` and `FailureStage`
  become compact.
- It avoids a protocol version bump and does not require shared-memory layout
  migration.

### Non-Goals

- Do not change `ResponseMessageKind`.
- Do not change `SlotRuntimeStateCode`.
- Do not change `Session::SessionState`.
- Do not change `SharedMemoryHeader`, `SlotRuntimeState`,
  `ResponseSlotRuntimeState`, or any protocol layout `static_assert`.

### Validation

- Rebuild the tree with the normal Clang/Ninja path.
- Run focused tests that cover:
  - API headers and typed enums
  - protocol layout invariants
  - replay/runtime-state mapping
  - recovery-policy and failure-report structures

## Phase 2: Protocol V4 Layout Rework

### Trigger

Do this only when we are willing to bump the shared-memory protocol version and
update both sides together.

### Main Objective

Get real layout wins instead of only changing enum declarations.

### Proposed Work Items

1. Introduce protocol v4 with an explicit migration boundary from v3.
2. Repack control and runtime metadata instead of shrinking enums in place.
3. Replace scattered 32-bit state fields with packed control words or tightly
   grouped byte fields where that actually reduces structure size.
4. Re-evaluate these structures together:
   - `SharedMemoryHeader`
   - `ResponseRingEntry`
   - `SlotRuntimeState`
   - `ResponseSlotRuntimeState`
5. Keep all protocol structs on explicit fixed-width integer fields and make
   padding/reserved bytes deliberate.
6. Update protocol layout tests to assert both offsets and total sizes for v4.
7. Audit all serialization/deserialization and state-cast sites after the field
   repack.

### Design Notes

- `ResponseMessageKind` should only move to `uint8_t` if the surrounding entry
  layout is repacked at the same time.
- `SlotRuntimeStateCode` should only move if the adjacent fields are reordered
  or packed so that the state byte does not immediately lose the gain to
  alignment padding.
- `Session::SessionState` should only change if `SharedMemoryHeader` is being
  repacked, because the current stored field is `uint32_t`.

### Suggested V4 Deliverables

- A protocol design note under `docs/`
- A single protocol v4 layout patch with size/offset assertions
- Compatibility decision recorded explicitly:
  - either hard cut to v4 only
  - or temporary dual-version attach logic during migration

## Recommended Order

1. Finish and verify Phase 1.
2. Capture baseline `sizeof` and `offsetof` data for current v3 structures.
3. Write the v4 layout note before editing protocol structs.
4. Implement v4 in one concentrated patch instead of piecemeal enum changes.
