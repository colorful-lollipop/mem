# Boundary Cleanup Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move all business opcodes and scan-related types out of `memrpc` core into app headers, preserving behavior.

**Architecture:** Keep `memrpc` transport-only. Apps (`minirpc`, `vps`) define their own opcode enums with explicit numeric values and convert to `memrpc::Opcode` via small helpers. Core keeps only generic enums/structs and shared-memory layout.

**Tech Stack:** C++17, CMake, GoogleTest

---

Execution note: run these tasks in a dedicated worktree.

### Task 1: Add MiniRpc Opcode Header And Migrate MiniRpc Usage

**Files:**
- Create: `include/apps/minirpc/common/minirpc_protocol.h`
- Modify: `tests/apps/minirpc/minirpc_headers_test.cpp`
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`

**Step 1: Write the failing test**

Add compile-time checks in `tests/apps/minirpc/minirpc_headers_test.cpp`:

```cpp
#include "apps/minirpc/common/minirpc_protocol.h"

static_assert(static_cast<uint16_t>(
    OHOS::Security::VirusProtectionService::MiniRpc::MiniOpcode::Echo) == 200);
static_assert(static_cast<uint16_t>(
    OHOS::Security::VirusProtectionService::MiniRpc::MiniOpcode::Add) == 201);
static_assert(static_cast<uint16_t>(
    OHOS::Security::VirusProtectionService::MiniRpc::MiniOpcode::Sleep) == 202);
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_headers_test`

Expected: FAIL with missing header or unknown `MiniOpcode`.

**Step 3: Write minimal implementation**

Create `include/apps/minirpc/common/minirpc_protocol.h`:

```cpp
#ifndef APPS_MINIRPC_COMMON_MINIRPC_PROTOCOL_H_
#define APPS_MINIRPC_COMMON_MINIRPC_PROTOCOL_H_

#include <cstdint>
#include "core/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

enum class MiniOpcode : uint16_t {
  Echo = 200,
  Add = 201,
  Sleep = 202,
  CrashForTest = 203,
};

inline constexpr memrpc::Opcode ToOpcode(MiniOpcode opcode) {
  return static_cast<memrpc::Opcode>(opcode);
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_COMMON_MINIRPC_PROTOCOL_H_
```

Update MiniRpc implementations to use `MiniOpcode`:

```cpp
server->RegisterHandler(ToOpcode(MiniOpcode::Echo), ...);
server->RegisterHandler(ToOpcode(MiniOpcode::Add), ...);
server->RegisterHandler(ToOpcode(MiniOpcode::Sleep), ...);
server->RegisterHandler(ToOpcode(MiniOpcode::CrashForTest), ...);
```

```cpp
return InvokeEncoded(&client_, request, ToOpcode(MiniOpcode::Echo));
```

**Step 4: Run tests to verify they pass**

Run:
- `cmake --build build --target memrpc_minirpc_headers_test memrpc_minirpc_client_test memrpc_minirpc_service_test`
- `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_(headers|client|service)_test'`

Expected: PASS.

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_protocol.h \
  tests/apps/minirpc/minirpc_headers_test.cpp \
  src/apps/minirpc/child/minirpc_service.cpp \
  src/apps/minirpc/parent/minirpc_async_client.cpp

git commit -m "chore: move minirpc opcodes to app header"
```

### Task 2: Add Vps Opcode Header And Migrate Vps Usage

**Files:**
- Create: `include/apps/vps/common/vps_protocol.h`
- Modify: `tests/apps/vps/vps_codec_test.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`
- Modify: `src/apps/vps/child/virus_engine_service.cpp`

**Step 1: Write the failing test**

Add compile-time checks in `tests/apps/vps/vps_codec_test.cpp`:

```cpp
#include "apps/vps/common/vps_protocol.h"

static_assert(static_cast<uint16_t>(
    OHOS::Security::VirusProtectionService::Vps::VpsOpcode::Init) == 100);
static_assert(static_cast<uint16_t>(
    OHOS::Security::VirusProtectionService::Vps::VpsOpcode::ScanFile) == 102);
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_vps_codec_test`

Expected: FAIL with missing header or unknown `VpsOpcode`.

**Step 3: Write minimal implementation**

Create `include/apps/vps/common/vps_protocol.h`:

```cpp
#ifndef APPS_VPS_COMMON_VPS_PROTOCOL_H_
#define APPS_VPS_COMMON_VPS_PROTOCOL_H_

#include <cstdint>
#include "core/protocol.h"

namespace OHOS::Security::VirusProtectionService::Vps {

enum class VpsOpcode : uint16_t {
  Init = 100,
  DeInit = 101,
  ScanFile = 102,
  ScanBehavior = 103,
  IsExistAnalysisEngine = 104,
  CreateAnalysisEngine = 105,
  DestroyAnalysisEngine = 106,
  UpdateFeatureLib = 107,
};

inline constexpr memrpc::Opcode ToOpcode(VpsOpcode opcode) {
  return static_cast<memrpc::Opcode>(opcode);
}

}  // namespace OHOS::Security::VirusProtectionService::Vps

#endif  // APPS_VPS_COMMON_VPS_PROTOCOL_H_
```

Update Vps usage:

```cpp
InvokeInt32(ToOpcode(VpsOpcode::Init), ...);
```

```cpp
server->RegisterHandler(ToOpcode(VpsOpcode::ScanFile), ...);
```

**Step 4: Run tests to verify they pass**

Run:
- `cmake --build build --target memrpc_vps_codec_test`
- `ctest --test-dir build --output-on-failure -R memrpc_vps_codec_test`

Expected: PASS.

**Step 5: Commit**

```bash
git add include/apps/vps/common/vps_protocol.h \
  tests/apps/vps/vps_codec_test.cpp \
  src/apps/vps/parent/virus_engine_manager.cpp \
  src/apps/vps/child/virus_engine_service.cpp

git commit -m "chore: move vps opcodes to app header"
```

### Task 3: Remove Business Types From Core And Update MemRpc Tests

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `include/memrpc/core/types.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/handler.h`
- Modify: `tests/memrpc/types_test.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`

**Step 1: Write the failing test**

Update `tests/memrpc/types_test.cpp` to expect a core opcode default:

```cpp
static_assert(static_cast<uint16_t>(memrpc::Opcode::Invalid) == 0);
```

Replace `Opcode::ScanFile` in memrpc tests with a local test opcode:

```cpp
constexpr memrpc::Opcode kTestOpcode =
    static_cast<memrpc::Opcode>(1000);
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_types_test`

Expected: FAIL because `Opcode::Invalid` does not exist yet.

**Step 3: Write minimal implementation**

Core cleanup:

```cpp
// src/core/protocol.h
enum class Opcode : uint16_t {
  Invalid = 0,
};
```

Remove app opcodes and legacy scan payload structs from `src/core/protocol.h`.

Update defaults to `Opcode::Invalid` in:
- `include/memrpc/client/rpc_client.h` (`RpcCall::opcode`)
- `include/memrpc/server/handler.h` (`RpcServerCall::opcode`)
- `src/core/protocol.h` (`RequestRingEntry::opcode`)

Remove business types from `include/memrpc/core/types.h`:
- `ScanRequest`
- `ScanBehaviorRequest`
- `ScanVerdict`
- `ScanResult`
- `ScanBehaviorResult`

Remove `RpcServerReply::verdict` and `IScanHandler` from `include/memrpc/server/handler.h`.

**Step 4: Run tests to verify they pass**

Run:
- `cmake --build build`
- `ctest --test-dir build --output-on-failure -R 'memrpc_(types|rpc_client_api|rpc_server_api|rpc_client_integration|response_queue_event)_test'`

Expected: PASS.

**Step 5: Commit**

```bash
git add src/core/protocol.h \
  include/memrpc/core/types.h \
  include/memrpc/client/rpc_client.h \
  include/memrpc/server/handler.h \
  tests/memrpc/types_test.cpp \
  tests/memrpc/rpc_client_api_test.cpp \
  tests/memrpc/rpc_server_api_test.cpp \
  tests/memrpc/rpc_client_integration_test.cpp \
  tests/memrpc/response_queue_event_test.cpp

git commit -m "chore: remove business types from memrpc core"
```
