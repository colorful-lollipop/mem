# Boundary Cleanup Design (MemRpc vs Apps)

## Goal
Move business-specific protocol and types out of the core framework so `memrpc` remains transport-only, while keeping current behavior unchanged.

## Non-Goals
- No behavior or performance changes.
- No refactor of data flow or threading.
- No new features or protocol versions.

## Chosen Approach
Minimal API change:
- Keep `memrpc::Opcode` type in core but remove all app-specific enumerators.
- Apps define their own opcode enums and convert to `memrpc::Opcode` via small helpers.

## Architecture
- Core `memrpc` keeps only transport primitives, shared memory layout, and generic request/response types.
- `apps/minirpc` owns `MiniOpcode` definitions.
- `apps/vps` owns `VpsOpcode` definitions and any legacy wire payload structs still needed.

## Component Changes
- New headers:
  - `include/apps/minirpc/common/minirpc_protocol.h`
  - `include/apps/vps/common/vps_protocol.h`
- Core cleanup:
  - `include/memrpc/core/types.h`: remove `Scan*` types and `ScanVerdict`.
  - `include/memrpc/server/handler.h`: remove `IScanHandler` and `RpcServerReply::verdict`.
  - `src/core/protocol.h`: remove app opcodes and legacy scan payload structs.
- Update `MiniRpc` code to use `MiniOpcode` helpers.
- Update `Vps` code to use `VpsOpcode` helpers.

## Data Flow
Unchanged. Only opcode ownership and header locations move. Encoders/decoders remain identical.

## Error Handling
Unchanged. All `StatusCode` handling stays as-is.

## Testing
- Update `tests/memrpc/*` to avoid removed `Opcode::ScanFile` and use a test-local opcode value.
- Adjust includes and opcode references in `tests/apps/minirpc/*` and `tests/apps/vps/*`.
- No new tests required beyond refactor touches.

## Risks and Mitigations
- Risk: widespread compile errors from removed symbols.
  - Mitigation: staged edits with incremental builds and test updates.
- Risk: opcode value mismatches across apps.
  - Mitigation: explicit enum values in app headers, keep existing numeric values.

## Compatibility
- Wire protocol and shared memory layout unchanged.
- Opcode numeric values preserved to avoid behavioral changes.
