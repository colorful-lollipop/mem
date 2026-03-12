# VpsDemo Tests + Fuzz Design (2026-03-12)

## Goals

- Expand vpsdemo unit coverage for codec, sample rules, heartbeat/health, and recovery policy behavior.
- Add a vpsdemo codec fuzz target that exercises Decode + EvaluateSamplePath and avoids crash/sleep hazards.
- Keep ohos_sa_mock out of new tests; reuse existing infra only in integration-style crash recovery test.

## Non-Goals

- No new registry tests.
- No new tests specifically for ohos_sa_mock.
- No heavy long-running stress suites.

## Proposed Approaches

### A) Unit + codec fuzz only
Pros: fast, stable. Cons: no crash-recovery path coverage.

### B) Unit + codec fuzz + crash-recovery integration (chosen)
Pros: covers real death/restart behavior. Cons: a bit heavier but bounded.

### C) Heavier scenario suites
Pros: broader coverage. Cons: slower/flakier.

## Architecture

- **Unit tests** under `demo/vpsdemo/tests/` for:
  - `vpsdemo_codec` (ScanFileRequest/Reply encode/decode).
  - `vpsdemo_sample_rules` (crash/virus/eicar/sleep rules + edge cases).
  - `vpsdemo_health` (in_flight/idle/last_task_age_ms snapshot).
  - `vpsdemo_policy` (exec timeout -> onFailure -> Restart decision).
  - heartbeat test enhancements (session_id/status/fields).
- **Integration test** (`vpsdemo_crash_recovery_test`) uses the real engine_sa + registry to verify crash -> callback -> restart -> scan ok.
- **Fuzz target** under `demo/vpsdemo/tests/fuzz/`:
  - Decode `ScanFileRequest` from fuzzer bytes.
  - Sanitize path to avoid `crash` and long `sleep` (replace/strip, or clamp to `sleep0`).
  - Call `EvaluateSamplePath` for rule coverage.
  - Optionally encode/decode `ScanFileReply` for codec coverage.

## Data Flow

- Unit: bytes <-> codec <-> assert; rules -> assert; health snapshot -> assert.
- Integration: spawn engine -> ScanFile -> crash -> wait for restart -> ScanFile ok.
- Fuzz: bytes -> Decode -> sanitize -> EvaluateSamplePath -> optional reply codec.

## Error Handling

- Codec tests assert Decode failure on malformed input.
- Fuzz target exits early on invalid decode; sanitized path avoids abort/sleep stalls.
- Integration uses bounded waits with clear failure signals.

## Testing Plan

- Unit tests under `VPSDEMO_ENABLE_TESTS`.
- Fuzz target under `VPSDEMO_ENABLE_FUZZ` with `-fsanitize=fuzzer,address,undefined`.
- Integration test labeled (e.g. `dt` or `integration`) to avoid default runs if desired.
