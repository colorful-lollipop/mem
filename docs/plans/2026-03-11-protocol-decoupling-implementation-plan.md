# Protocol Decoupling Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `memrpc` 核心协议与应用协议拆分，核心仅保留框架线协议，应用各自维护 opcode/payload。

**Architecture:** 核心协议只定义 shared-memory 布局与通用头结构，`opcode` 视为 `uint16_t`。VPS/MiniRPC 分别在 `include/apps/<app>/protocol.h` 中定义自己的 `enum class` 与 payload，并由各自应用代码引用。

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: 新增应用协议头 + 头文件用例

**Files:**
- Create: `include/apps/vps/protocol.h`
- Create: `include/apps/minirpc/protocol.h`
- Modify: `tests/apps/minirpc/minirpc_headers_test.cpp`
- Create: `tests/apps/vps/vps_headers_test.cpp`
- Modify: `tests/apps/vps/CMakeLists.txt`

**Step 1: 写一个会失败的头文件测试（MiniRPC）**

在 `tests/apps/minirpc/minirpc_headers_test.cpp` 里新增：
```cpp
#include "apps/minirpc/protocol.h"

using OHOS::Security::VirusProtectionService::MiniRpc::MiniRpcOpcode;
static_assert(static_cast<uint16_t>(MiniRpcOpcode::MiniEcho) == 200u);
```

**Step 2: 跑测试确认失败**

Run: `cmake --build build`
Expected: 编译失败，提示找不到 `apps/minirpc/protocol.h` 或 `MiniRpcOpcode` 未定义。

**Step 3: 写一个会失败的头文件测试（VPS）**

创建 `tests/apps/vps/vps_headers_test.cpp`：
```cpp
#include <gtest/gtest.h>
#include <type_traits>

#include "apps/vps/protocol.h"

using OHOS::Security::VirusProtectionService::VpsOpcode;

TEST(VpsHeadersTest, ProtocolHeaderCompiles) {
  EXPECT_TRUE((std::is_enum_v<VpsOpcode>));
  EXPECT_EQ(static_cast<uint16_t>(VpsOpcode::VpsInit), 100u);
}
```

在 `tests/apps/vps/CMakeLists.txt` 增加：
```cmake
vps_add_test(memrpc_vps_headers_test vps_headers_test.cpp)
```

**Step 4: 实现新协议头**

`include/apps/minirpc/protocol.h`：
```cpp
namespace OHOS::Security::VirusProtectionService::MiniRpc {

enum class MiniRpcOpcode : uint16_t {
  MiniEcho = 200,
  MiniAdd = 201,
  MiniSleep = 202,
  MiniCrashForTest = 203,
  MiniHangForTest = 204,
  MiniOomForTest = 205,
  MiniStackOverflowForTest = 206,
};

}  // namespace ...::MiniRpc
```

`include/apps/vps/protocol.h`：
```cpp
namespace OHOS::Security::VirusProtectionService {

enum class VpsOpcode : uint16_t {
  VpsInit = 100,
  VpsDeInit = 101,
  VpsScanFile = 102,
  VpsScanBehavior = 103,
  VpsIsExistAnalysisEngine = 104,
  VpsCreateAnalysisEngine = 105,
  VpsDestroyAnalysisEngine = 106,
  VpsUpdateFeatureLib = 107,
};

inline constexpr uint32_t VPS_MAX_FILE_PATH_SIZE = 1024u;
inline constexpr uint32_t VPS_MAX_MESSAGE_SIZE = 512u;

struct ScanFileRequestPayload {
  uint32_t file_path_length = 0;
  char file_path[VPS_MAX_FILE_PATH_SIZE]{};
};

struct ScanFileResponsePayload {
  uint32_t verdict = 0;
  uint32_t message_length = 0;
  char message[VPS_MAX_MESSAGE_SIZE]{};
};

}  // namespace OHOS::Security::VirusProtectionService
```

**Step 5: 重新编译验证通过**

Run: `cmake --build build`
Expected: 编译通过。

**Step 6: Commit**

```bash
git add include/apps/vps/protocol.h include/apps/minirpc/protocol.h \
  tests/apps/minirpc/minirpc_headers_test.cpp \
  tests/apps/vps/vps_headers_test.cpp tests/apps/vps/CMakeLists.txt

git commit -m "feat: add app protocol headers"
```

---

### Task 2: 核心协议移除应用枚举与 payload

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `include/memrpc/server/handler.h`
- Modify: `include/memrpc/server/rpc_server.h`
- Modify: `include/memrpc/server/typed_handler.h`
- Modify: `include/memrpc/client/typed_invoker.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `src/server/rpc_server.cpp`

**Step 1: 写一个会失败的编译变更**

在 `src/core/protocol.h` 先移除 `enum class Opcode` 与 `ScanFile*Payload` 定义，暂时不改其它文件。

**Step 2: 编译确认失败**

Run: `cmake --build build`
Expected: 失败，提示 `Opcode` 未定义或相关引用缺失。

**Step 3: 引入 core 级 opcode 类型并修复引用**

- 在 `src/core/protocol.h` 增加：
```cpp
using Opcode = uint16_t;
inline constexpr Opcode OPCODE_INVALID = 0;
```
- 将所有默认初始化：`Opcode::ScanFile` 替换为 `OPCODE_INVALID`。
- 将 `RpcCall`, `RpcFailure`, `RpcServerCall` 等字段改为 `Opcode`（实际为 `uint16_t`）。
- `RegisterHandler` / `InvokeTyped` / `RegisterTypedHandler` 参数改为 `Opcode`。
- `InvokeTyped` 内部使用 `call.opcode = static_cast<Opcode>(opcode);`（如果模板参数仍是 `Opcode`，可直接赋值）。

**Step 4: 编译验证通过**

Run: `cmake --build build`
Expected: 编译通过。

**Step 5: Commit**

```bash
git add src/core/protocol.h \
  include/memrpc/client/rpc_client.h \
  include/memrpc/server/handler.h \
  include/memrpc/server/rpc_server.h \
  include/memrpc/server/typed_handler.h \
  include/memrpc/client/typed_invoker.h \
  src/client/rpc_client.cpp src/server/rpc_server.cpp

git commit -m "feat: decouple core opcode type"
```

---

### Task 3: 更新 MiniRPC 应用与测试使用新 opcode

**Files:**
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Modify: `tests/apps/minirpc/*.cpp`
- Modify: `tests/memrpc/rpc_client_timeout_watchdog_test.cpp`
- Modify: `tests/memrpc/rpc_client_idle_callback_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_stress_runner.cpp`
- Modify: `tests/apps/minirpc/minirpc_resilient_invoker.h`

**Step 1: 写一个会失败的替换（MiniRPC）**

把一处 `MemRpc::Opcode::MiniEcho` 改成 `MiniRpcOpcode::MiniEcho`，先不加 include。

**Step 2: 编译确认失败**

Run: `cmake --build build`
Expected: 失败，提示 `MiniRpcOpcode` 未定义或缺少 include。

**Step 3: 全量替换并补 include**

- 在 MiniRPC 相关文件中增加 `#include "apps/minirpc/protocol.h"`。
- 全量替换 `MemRpc::Opcode::Mini*` 为 `MiniRpcOpcode::Mini*`。
- 如果需要，将 `Opcode` 字段赋值改为：
```cpp
call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniEcho);
```
（或直接 `call.opcode = static_cast<uint16_t>(MiniRpcOpcode::MiniEcho);`）

**Step 4: 运行 MiniRPC 相关测试**

Run: `ctest --test-dir build -R memrpc_minirpc_ --output-on-failure`
Expected: 全部通过。

**Step 5: Commit**

```bash
git add src/apps/minirpc \
  tests/apps/minirpc \
  tests/memrpc/rpc_client_timeout_watchdog_test.cpp \
  tests/memrpc/rpc_client_idle_callback_test.cpp \
  tests/apps/minirpc/minirpc_resilient_invoker.h

git commit -m "feat: switch minirpc to app opcode"
```

---

### Task 4: 更新 VPS 应用与 demo 使用新 opcode

**Files:**
- Modify: `src/apps/vps/child/virus_engine_service.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.h`
- Modify: `src/apps/vps/common/vps_codec.cpp`
- Modify: `demo/vpsdemo/src/vpsdemo_client.cpp`
- Modify: `include/apps/vps/parent/virus_engine_manager.h`
- Modify: `include/apps/vps/child/virus_engine_service.h`

**Step 1: 先做一次会失败的替换**

把 `memrpc::Opcode::VpsInit` 改为 `VpsOpcode::VpsInit`，先不加 include。

**Step 2: 编译确认失败**

Run: `cmake --build build`
Expected: 失败，提示 `VpsOpcode` 未定义或缺少 include。

**Step 3: 全量替换并补 include**

- 在 VPS 相关文件中增加 `#include "apps/vps/protocol.h"`。
- 全量替换 `memrpc::Opcode::Vps*` 为 `VpsOpcode::Vps*`。
- 将 `memrpc::Opcode` 类型参数改为 `MemRpc::Opcode`（uint16_t），赋值时加 cast：
```cpp
call.opcode = static_cast<MemRpc::Opcode>(VpsOpcode::VpsScanFile);
```

**Step 4: 运行 VPS 相关测试**

Run: `ctest --test-dir build -R memrpc_vps_ --output-on-failure`
Expected: 全部通过。

**Step 5: Commit**

```bash
git add src/apps/vps demo/vpsdemo include/apps/vps

git commit -m "feat: switch vps to app opcode"
```

---

### Task 5: 更新框架测试使用测试 opcode

**Files:**
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/rpc_server_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: 定义测试 opcode 常量**

在每个测试文件顶层增加：
```cpp
constexpr memrpc::Opcode kTestOpcode = 1u;
```
并替换所有 `memrpc::Opcode::ScanFile` 为 `kTestOpcode`。

**Step 2: 运行框架测试**

Run: `ctest --test-dir build -R memrpc_ --output-on-failure`
Expected: 全部通过。

**Step 3: Commit**

```bash
git add tests/memrpc

git commit -m "feat: update memrpc tests for raw opcode"
```

---

### Task 6: 全量构建与回归检查

**Files:**
- None

**Step 1: 全量构建**

Run: `cmake --build build`
Expected: 编译通过。

**Step 2: 全量测试**

Run: `ctest --test-dir build --output-on-failure`
Expected: 全部通过。

**Step 3: Commit（若有遗漏修复）**

```bash
git add -A

git commit -m "fix: follow-up protocol decoupling cleanup"
```
