## Summary

Add shared sample-evaluation rules for vpsdemo scan paths and a standalone stress client
that issues randomized concurrent scans and validates results deterministically.

## Goals

- Deterministic, shared rules for mapping file paths to scan behavior/results.
- Manual stress executable that exercises concurrency and validates correctness.
- Keep existing "virus substring => threat=1" behavior for compatibility.

## Non-Goals

- No changes to core memrpc framework or transport.
- No CI/GTest additions (manual-only executable).

## Sample Rules (Shared)

Add `vpsdemo_sample_rules.{h,cpp}` with:

- `struct SampleBehavior { int threatLevel; uint32_t sleepMs; bool shouldCrash; };`
- `SampleBehavior EvaluateSamplePath(const std::string& path);`

Rule order:
1. If `path` contains `"crash"`: `shouldCrash=true`.
2. If `path` contains `"sleep<digits>"`: `sleepMs = min(digits * 1000, MAX_SLEEP_MS)`.
3. If `path` contains `"virus"` or `"eicar"`: `threatLevel=1`, else `0`.

`MAX_SLEEP_MS` defaults to 5000.

## Service Changes

In `VpsDemoService::RegisterHandlers` (ScanFile handler):

- Call `EvaluateSamplePath(request.file_path)`.
- If `shouldCrash`, log then `abort()` (explicit, manual-only).
- If `sleepMs > 0`, sleep for that duration.
- Return `threatLevel` as the scan result; `code` remains `0` when initialized.

## Stress Client

Add executable `vpsdemo_stress_client`:

- CLI flags:
  - `--threads N` (default 8)
  - `--iterations M` (default 200)
  - `--seed S` (default time-based)
  - `--include-crash` (default off)
- Generate randomized paths from a fixed pool:
  - `/data/clean_<rand>.apk`
  - `/data/virus_<rand>.apk`
  - `/data/eicar_<rand>.apk`
  - `/data/sleep3_<rand>.apk`
  - `/data/sleep100_<rand>.apk`
  - `/data/crash_<rand>.apk` (only when `--include-crash`)
- For each request:
  - Compute expected behavior via `EvaluateSamplePath`.
  - If `shouldCrash` and `--include-crash` not set, skip.
  - Issue `ScanFile`; validate `threat_level` matches expectation.
- Track `ok`, `mismatch`, `rpc_error`; return non-zero on any mismatch/error.

## Error Handling

- RPC errors are counted and cause non-zero exit.
- Mismatches include expected vs actual in logs.

## Testing

- Manual only: run `vpsdemo_supervisor` in one terminal, then
  `vpsdemo_stress_client --threads 8 --iterations 200`.
