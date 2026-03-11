# Minirpc Demo Restructure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move MiniRpc into `demo/minirpc` with `minirpc/...` headers, and make both `demo/minirpc` and `demo/vpsdemo` build from root while sharing a single framework target.

**Architecture:** Keep the framework as source-only by defining a single `memrpc` library at the root and aliasing it as `memrpc_core` for demos. `demo/minirpc` and `demo/vpsdemo` become independent entries with guards to reuse `memrpc_core` when built from root, and each can build standalone.

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: Move MiniRpc headers/sources under `demo/minirpc`

**Files:**
- Move: `include/apps/minirpc/` -> `demo/minirpc/include/minirpc/`
- Move: `src/apps/minirpc/` -> `demo/minirpc/src/apps/minirpc/`
- Move: `demo/minirpc_demo_main.cpp` -> `demo/minirpc/src/minirpc_demo_main.cpp`

**Step 1: Create destination directories**

Run:
```bash
mkdir -p demo/minirpc/include demo/minirpc/src
```
Expected: directories exist.

**Step 2: Move MiniRpc headers**

Run:
```bash
git mv include/apps/minirpc demo/minirpc/include/minirpc
```
Expected: Git records the rename.

**Step 3: Move MiniRpc sources**

Run:
```bash
git mv src/apps/minirpc demo/minirpc/src/apps/minirpc
```
Expected: Git records the rename.

**Step 4: Move demo main**

Run:
```bash
git mv demo/minirpc_demo_main.cpp demo/minirpc/src/minirpc_demo_main.cpp
```
Expected: Git records the rename.

**Step 5: Update includes in moved sources to `minirpc/...`**

Run:
```bash
rg -l "apps/minirpc/" demo/minirpc/src demo/minirpc/include \
  | xargs perl -pi -e 's#apps/minirpc/#minirpc/#g'
```
Expected: `rg "apps/minirpc/" demo/minirpc` returns no matches.

**Step 6: Commit**

Run:
```bash
git add demo/minirpc include/apps src/apps demo

git commit -m "feat: move minirpc sources under demo"
```
Expected: commit created with moved files.

---

### Task 2: Update MiniRpc include usage across the repo

**Files:**
- Modify: `tests/**` that include `apps/minirpc/...`
- Modify: any `src/` or `demo/` files still referencing `apps/minirpc/...`

**Step 1: Replace remaining include prefixes**

Run:
```bash
rg -l "apps/minirpc/" tests src demo \
  | xargs perl -pi -e 's#apps/minirpc/#minirpc/#g'
```
Expected: include paths updated.

**Step 2: Verify no stale include prefixes**

Run:
```bash
rg -n "apps/minirpc/" tests src demo
```
Expected: no matches.

**Step 3: Commit**

Run:
```bash
git add tests src demo

git commit -m "feat: update minirpc include paths"
```
Expected: commit created with include updates.

---

### Task 3: Rewire root and framework CMake targets

**Files:**
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Delete: `demo/CMakeLists.txt` (no longer needed)

**Step 1: Remove the old `minirpc_demo` library and add alias**

Edit `src/CMakeLists.txt`:
- Delete the `minirpc_demo` target block.
- Add alias after `memrpc` definition:
```cmake
add_library(memrpc_core ALIAS memrpc)
```

**Step 2: Update root CMake to add the two demo entries**

Edit `CMakeLists.txt`:
- Remove `add_subdirectory(tests)`.
- Replace `add_subdirectory(demo)` with:
```cmake
add_subdirectory(demo/minirpc)
add_subdirectory(demo/vpsdemo)
```

**Step 3: Remove the obsolete `demo/CMakeLists.txt`**

Run:
```bash
git rm demo/CMakeLists.txt
```
Expected: file removed from git.

**Step 4: Commit**

Run:
```bash
git add src/CMakeLists.txt CMakeLists.txt demo/CMakeLists.txt

git commit -m "feat: wire demo entries and memrpc_core alias"
```
Expected: commit created with CMake updates.

---

### Task 4: Create the MiniRpc entry CMake and test entrypoint

**Files:**
- Create: `demo/minirpc/CMakeLists.txt`
- Move: `tests/CMakeLists.txt` -> `demo/minirpc/tests/CMakeLists.txt`

**Step 1: Add `demo/minirpc/CMakeLists.txt`**

Create `demo/minirpc/CMakeLists.txt` with:
```cmake
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(minirpc LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(MEMRPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(NOT TARGET memrpc_core)
  add_library(memrpc_core
    ${MEMRPC_ROOT}/src/client/rpc_client.cpp
    ${MEMRPC_ROOT}/src/server/rpc_server.cpp
    ${MEMRPC_ROOT}/src/bootstrap/posix_demo_bootstrap.cpp
    ${MEMRPC_ROOT}/src/bootstrap/sa_bootstrap.cpp
    ${MEMRPC_ROOT}/src/core/byte_reader.cpp
    ${MEMRPC_ROOT}/src/core/byte_writer.cpp
    ${MEMRPC_ROOT}/src/core/log.cpp
    ${MEMRPC_ROOT}/src/core/slot_pool.cpp
    ${MEMRPC_ROOT}/src/core/session.cpp
  )
  target_include_directories(memrpc_core PUBLIC
    ${MEMRPC_ROOT}/include
    ${MEMRPC_ROOT}/src
  )
  target_compile_features(memrpc_core PUBLIC cxx_std_17)
  add_library(memrpc ALIAS memrpc_core)
endif()

add_library(minirpc
  src/apps/minirpc/child/minirpc_service.cpp
  src/apps/minirpc/parent/minirpc_async_client.cpp
  src/apps/minirpc/parent/minirpc_client.cpp
)

target_include_directories(minirpc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(minirpc PUBLIC memrpc_core)
target_compile_features(minirpc PUBLIC cxx_std_17)

add_executable(memrpc_minirpc_demo src/minirpc_demo_main.cpp)
target_link_libraries(memrpc_minirpc_demo PRIVATE minirpc)

add_subdirectory(tests)
```

**Step 2: Move top-level tests CMake**

Run:
```bash
git mv tests/CMakeLists.txt demo/minirpc/tests/CMakeLists.txt
```
Expected: file moved.

**Step 3: Update moved test root CMake**

Edit `demo/minirpc/tests/CMakeLists.txt` to:
- `add_subdirectory(memrpc)`
- `add_subdirectory(minirpc)`
- `add_subdirectory(fuzz)` guarded by `MEMRPC_ENABLE_FUZZ`
- Keep existing options for DT/stress.

**Step 4: Commit**

Run:
```bash
git add demo/minirpc/CMakeLists.txt demo/minirpc/tests/CMakeLists.txt

git commit -m "feat: add minirpc entry CMake and test root"
```
Expected: commit created with new entry CMake.

---

### Task 5: Move tests into `demo/minirpc/tests` and fix target links

**Files:**
- Move: `tests/memrpc/` -> `demo/minirpc/tests/memrpc/`
- Move: `tests/apps/minirpc/` -> `demo/minirpc/tests/minirpc/`
- Move: `tests/fuzz/` -> `demo/minirpc/tests/fuzz/`
- Modify: `demo/minirpc/tests/memrpc/CMakeLists.txt`
- Modify: `demo/minirpc/tests/minirpc/CMakeLists.txt`
- Modify: `demo/minirpc/tests/fuzz/CMakeLists.txt`

**Step 1: Move test directories**

Run:
```bash
git mv tests/memrpc demo/minirpc/tests/memrpc

git mv tests/apps/minirpc demo/minirpc/tests/minirpc

git mv tests/fuzz demo/minirpc/tests/fuzz
```
Expected: directories moved under `demo/minirpc/tests`.

**Step 2: Update test CMake link targets**

Edit `demo/minirpc/tests/memrpc/CMakeLists.txt`:
- Replace `target_link_libraries(${target} PRIVATE memrpc ...)` with `memrpc_core`.

Edit `demo/minirpc/tests/minirpc/CMakeLists.txt`:
- Replace `target_link_libraries(${target} PRIVATE minirpc_demo ...)` with `minirpc`.

Edit `demo/minirpc/tests/fuzz/CMakeLists.txt`:
- Replace `target_link_libraries(... minirpc_demo)` with `minirpc`.

**Step 3: Commit**

Run:
```bash
git add demo/minirpc/tests

git commit -m "feat: move minirpc tests under demo"
```
Expected: commit created with relocated tests and updated link targets.

---

### Task 6: Fix path-based tests and include paths

**Files:**
- Modify: `demo/minirpc/tests/memrpc/build_config_test.cpp`
- Modify: `demo/minirpc/tests/memrpc/framework_split_headers_test.cpp`

**Step 1: Update build config test paths**

Edit `demo/minirpc/tests/memrpc/build_config_test.cpp`:
- Update references to:
  - `tests/CMakeLists.txt` -> `demo/minirpc/tests/CMakeLists.txt`
  - `tests/memrpc/CMakeLists.txt` -> `demo/minirpc/tests/memrpc/CMakeLists.txt`
  - `tests/apps/minirpc/CMakeLists.txt` -> `demo/minirpc/tests/minirpc/CMakeLists.txt`
  - `demo/CMakeLists.txt` -> `demo/minirpc/CMakeLists.txt`
- Replace string checks for `add_subdirectory(apps/minirpc)` with `add_subdirectory(minirpc)`.

**Step 2: Update framework split headers test**

Edit `demo/minirpc/tests/memrpc/framework_split_headers_test.cpp`:
- Replace include prefixes with `minirpc/...`.
- Update file path for header check to:
  - `/root/code/demo/mem/demo/minirpc/include/minirpc/parent/minirpc_client.h`

**Step 3: Commit**

Run:
```bash
git add demo/minirpc/tests/memrpc/build_config_test.cpp demo/minirpc/tests/memrpc/framework_split_headers_test.cpp

git commit -m "fix: update minirpc test path assertions"
```
Expected: commit created with test path updates.

---

### Task 7: Update vpsdemo CMake to reuse shared framework target

**Files:**
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Guard `cmake_minimum_required` and `project`**

Wrap with top-level guard:
```cmake
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(vpsdemo LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()
```

**Step 2: Reuse `memrpc_core` if present**

Update the `memrpc_core` block to:
```cmake
if(NOT TARGET memrpc_core)
  add_library(memrpc_core
      ${MEMRPC_ROOT}/src/client/rpc_client.cpp
      ${MEMRPC_ROOT}/src/server/rpc_server.cpp
      ${MEMRPC_ROOT}/src/bootstrap/posix_demo_bootstrap.cpp
      ${MEMRPC_ROOT}/src/bootstrap/sa_bootstrap.cpp
      ${MEMRPC_ROOT}/src/core/byte_reader.cpp
      ${MEMRPC_ROOT}/src/core/byte_writer.cpp
      ${MEMRPC_ROOT}/src/core/log.cpp
      ${MEMRPC_ROOT}/src/core/slot_pool.cpp
      ${MEMRPC_ROOT}/src/core/session.cpp
  )
  target_include_directories(memrpc_core PUBLIC
      ${MEMRPC_ROOT}/include
      ${MEMRPC_ROOT}/src
  )
  target_compile_features(memrpc_core PUBLIC cxx_std_17)
  target_link_libraries(memrpc_core PUBLIC pthread rt)
  add_library(memrpc ALIAS memrpc_core)
endif()
```

**Step 3: Commit**

Run:
```bash
git add demo/vpsdemo/CMakeLists.txt

git commit -m "feat: reuse memrpc_core in vpsdemo"
```
Expected: commit created with vpsdemo CMake guard.

---

### Task 8: Verification

**Step 1: Configure/build from root**

Run:
```bash
cmake -S . -B build
cmake --build build
```
Expected: build succeeds; both demo targets built.

**Step 2: Run root tests**

Run:
```bash
ctest --test-dir build --output-on-failure
```
Expected: tests under `demo/minirpc/tests` run and pass.

**Step 3: Configure/build from `demo/minirpc`**

Run:
```bash
cmake -S demo/minirpc -B build-minirpc
cmake --build build-minirpc
ctest --test-dir build-minirpc --output-on-failure
```
Expected: MiniRpc standalone build/tests succeed.

---

