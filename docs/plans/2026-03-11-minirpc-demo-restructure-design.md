# TODO: Minirpc Demo Restructure Design

## Context
The repository currently splits MiniRpc across `include/apps/minirpc`, `src/apps/minirpc`, `demo/`, and `tests/`. The desired direction is to treat demo apps as the only build entry points (framework is not built standalone), and to give MiniRpc a dedicated demo root that owns its own tests. MiniRpc headers should move to a new `minirpc/...` include prefix. Vpsdemo remains a separate demo entry point and will own its own tests later.

## Goals
- Move MiniRpc into `demo/minirpc/` with a dedicated build entry.
- Change all MiniRpc includes to the `minirpc/...` prefix.
- Keep framework headers at `memrpc/...` and treat framework as source-only (not a standalone project).
- Support both: building from the repo root, and building `demo/minirpc` standalone.
- Move MiniRpc-related tests (including framework tests and fuzz) under `demo/minirpc/tests` and build them from the MiniRpc entry.

## Non-Goals
- Redesign MiniRpc APIs or behavior.
- Create a standalone framework build artifact outside demo entry points.
- Refactor test semantics beyond path/layout changes.
- Move vpsdemo tests in this change (tracked separately).

## Chosen Approach
Adopt a shared `memrpc_core` target when building from the repo root, while keeping each demo entry independently buildable. The root build acts as a thin aggregator; the two demos define their own targets and can be built separately. Framework tests run once from the MiniRpc entry, not duplicated for vpsdemo.

## Proposed Layout
- `demo/minirpc/`
  - `CMakeLists.txt` (MiniRpc entry)
  - `include/minirpc/` (public headers, new include prefix)
  - `src/` (MiniRpc sources and demo main)
  - `tests/`
    - `memrpc/` (framework unit tests)
    - `minirpc/` (MiniRpc-specific tests)
    - `fuzz/` (MiniRpc fuzz targets)
- `demo/vpsdemo/` remains as-is (independent entry), tests to be added later.

## Build Strategy
### Root Build (aggregator)
- Keep root `CMakeLists.txt` but remove standalone framework/test entry points.
- Define `memrpc_core` once (as today in `src/CMakeLists.txt`), then add:
  - `add_subdirectory(demo/minirpc)`
  - `add_subdirectory(demo/vpsdemo)`
- MiniRpc and vpsdemo targets both link to `memrpc_core`.

### MiniRpc Entry (`demo/minirpc`)
- Provide a `memrpc_core` fallback if the target is not already defined:
  - `if(NOT TARGET memrpc_core)` then add library from `src/` files, include `include/` and `src/`.
- Build a `minirpc` library from `demo/minirpc/src/` sources.
- Build `memrpc_minirpc_demo` from `demo/minirpc/src/minirpc_demo_main.cpp`.
- Add `demo/minirpc/tests` via `add_subdirectory(tests)`; tests link to `memrpc_core` and `minirpc`.

### Vpsdemo Entry (`demo/vpsdemo`)
- Retain current standalone build structure.
- If built from root, switch to reusing the already-defined `memrpc_core` target (guarded by `if(TARGET memrpc_core)` in `demo/vpsdemo/CMakeLists.txt`).

## Headers and Includes
- Move MiniRpc headers from `include/apps/minirpc` to `demo/minirpc/include/minirpc`.
- Update all source and test includes from `apps/minirpc/...` to `minirpc/...`.
- Update include directories accordingly:
  - `demo/minirpc` targets add `${CMAKE_CURRENT_SOURCE_DIR}/include`.

## Tests
- Move:
  - `tests/memrpc/` -> `demo/minirpc/tests/memrpc/`
  - `tests/apps/minirpc/` -> `demo/minirpc/tests/minirpc/`
  - `tests/fuzz/` -> `demo/minirpc/tests/fuzz/`
- Update any tests that validate file paths (e.g. `build_config_test.cpp`, `framework_split_headers_test.cpp`) to the new locations.
- Keep existing test target names when possible to minimize downstream impact.

## Risks and Mitigations
- **CMake target name collisions**: avoid by defining `memrpc_core` once at root and using `if(NOT TARGET memrpc_core)` guards in demo entries.
- **Hard-coded path tests**: adjust string checks to new locations and include prefixes.
- **Standalone demo builds**: ensure `demo/minirpc` still builds without requiring a root configure step.

## Validation Plan
- Configure/build from repo root.
- Configure/build from `demo/minirpc` directly.
- Run `ctest` in the root build and in the `demo/minirpc` build.

