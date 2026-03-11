# Protocol Decoupling Design (memrpc vs. app protocols)

## Background
`src/core/protocol.h` currently mixes two layers:
- **Framework wire/layout**: shared-memory header magic, protocol version, ring/slot structs, sizes.
- **Application protocol**: VPS/MiniRPC opcodes and payload structs.

This couples framework changes to app-specific details and makes it harder to reuse the framework with multiple apps.

## Goals
- Keep `memrpc` core protocol **framework-only**.
- Move **app opcodes and payload structs** into per-app headers under `include/apps/<app>/`.
- Maintain clear ownership: core code includes only `memrpc/*` headers; apps include their own protocol headers.

## Non-Goals
- Backward compatibility for existing demos.
- Introducing complex registries or range enforcement for opcodes.

## Proposed Approach (Selected)
**Simple split (方案 1)**
- `memrpc` core protocol stays in `src/core/protocol.h` and contains only:
  - magic/version constants
  - ring/slot structs
  - request/response headers
  - slot size calculations
- Each app defines:
  - its own `enum class <App>Opcode : uint16_t`
  - request/response payload structs
  - any app-specific constants (e.g., max file path size)

### New/Adjusted Headers
- `include/apps/vps/protocol.h`
  - VPS opcodes + payload structs (e.g., ScanFileRequestPayload)
- `include/apps/minirpc/protocol.h`
  - MiniRPC opcodes + payload structs
- `src/core/protocol.h`
  - **Remove** app enums/payloads; keep only framework wire layout

### Design Notes
- Core remains application-agnostic: it only treats `opcode` as a `uint16_t`.
- Apps can assume their opcode spaces are independent; no registry required.
- No protocol version bump needed because wire layout remains unchanged.

## Data Flow Impact
- **Before**: app code includes `core/protocol.h` for opcodes/payloads.
- **After**: app code includes `apps/<app>/protocol.h`. Core code never includes app headers.

## Error Handling
No new runtime behavior. Compile-time separation only. Build errors will surface if app code still depends on core enums or payload structs.

## Testing
- Update existing tests that reference `memrpc::Opcode` or app payloads.
- Keep core tests compiling against pure framework headers.
- Ensure app tests include their new protocol headers and pass.

## Migration Scope
- Update includes in:
  - VPS app sources
  - MiniRPC app sources
  - tests that use app opcodes/payloads
- Remove app enums/payloads from `src/core/protocol.h`.

## Risks
- Missing include updates could cause build errors.
- App tests might still reference `memrpc::Opcode` — must be corrected.

## Outcome
Framework protocol becomes reusable, and each app owns its own protocol definition without coupling to `memrpc` core.
