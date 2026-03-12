# Design: CMake-to-GN Parity (Coexisting Builds)

## Goals
- Keep CMake and GN builds coexisting without removing or altering CMake behavior.
- Provide GN targets that match CMake outputs and options:
  - Libraries: `memrpc`, `minirpc_demo`
  - Executable: `memrpc_minirpc_demo`
  - Tests: all memrpc + minirpc tests
  - Feature toggles: `MEMRPC_ENABLE_STRESS_TESTS`, `MEMRPC_ENABLE_FUZZ`, `MEMRPC_ENABLE_DT_TESTS`
- Preserve current test expectations that read `CMakeLists.txt` directly.

## Non-Goals
- Replacing or deleting CMake files.
- Refactoring source layout or test contents.
- Introducing new build systems or additional tooling.

## Approach Options Considered
1. Single root `BUILD.gn` (fast but unmaintainable).
2. Directory-aligned GN files (recommended): mirror CMake subdirectories with targeted `BUILD.gn` files.
3. Minimal edits to existing `BUILD.gn` only (low change but hard to scale).

## Chosen Approach
Use directory-aligned `BUILD.gn` files that mirror CMake structure for clarity and maintainability.

## GN Layout
- `BUILD.gn` (root):
  - Global configs (`memrpc_config`, `memrpc_test_config`)
  - Import `memrpc.gni` args
  - Define top-level targets and/or forward to subdirectories
- `src/BUILD.gn`:
  - `source_set("memrpc")`
  - `source_set("minirpc_demo")`
- `demo/BUILD.gn`:
  - `executable("memrpc_minirpc_demo")`
- `tests/memrpc/BUILD.gn`:
  - memrpc tests with shared template and DT toggles
- `tests/apps/minirpc/BUILD.gn`:
  - minirpc tests and stress targets
- `tests/fuzz/BUILD.gn`:
  - fuzz target with Clang requirement and sanitizer flags

## Build Args
Extend `memrpc.gni` to define GN args that mirror CMake options:
- `memrpc_enable_tests` (existing)
- `memrpc_enable_stress_tests` (new)
- `memrpc_enable_fuzz` (new)
- `memrpc_enable_dt_tests` (new)
- `memrpc_gtest_main_target` (existing)

## Behavior Parity Details
- C++17 enforced via `-std=c++17` in configs and per-target when needed.
- Include paths: `include/`, `src/`, and `tests/` (only for stress config target).
- Special test defines:
  - `memrpc_rpc_client_timeout_watchdog_test` sets `MEMRPC_ASYNC_WATCHDOG_INTERVAL_MS=20`.
- Fuzz target:
  - Requires Clang (`assert(is_clang)` when enabled).
  - Uses `-fsanitize=fuzzer,address,undefined` for compile and link.
- Tests use `testonly = true` and depend on `memrpc_gtest_main_target`.

## Testing Strategy
- GN builds should be validated by building all targets with toggles on/off and running gtest binaries manually or via GN/Ninja conventions.
- CMake builds remain unchanged and can still be used with `ctest` for the existing suite.

## Files to Add or Update
- Update: `BUILD.gn`, `memrpc.gni`
- Add: `src/BUILD.gn`, `demo/BUILD.gn`, `tests/memrpc/BUILD.gn`, `tests/apps/minirpc/BUILD.gn`, `tests/fuzz/BUILD.gn`

