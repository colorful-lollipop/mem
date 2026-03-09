# MiniRpc Codec Template Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 MiniRpc codec 从按消息名展开的显式函数接口切换为 `CodecTraits<T> + EncodeMessage<T>/DecodeMessage<T>` 的模板接口，并同步迁移所有调用点。

**Architecture:** 在 `minirpc_codec.h` 中定义模板入口和各消息类型的显式特化，保留显式字段读写顺序以维持协议可读性；`MiniRpcAsyncClient` 和 `MiniRpcClient` 统一按消息类型调用模板接口；旧的 `EncodeXxx/DecodeXxx` API 全部移除。

**Tech Stack:** C++17、ByteWriter、ByteReader、GTest、CMake

---

### Task 1: 先将 codec 测试切到模板 API

**Files:**
- Modify: `tests/apps/minirpc/minirpc_codec_test.cpp`

**Step 1: Write the failing test**

把现有测试中的 `EncodeXxx()` / `DecodeXxx()` 调用改成：

- `EncodeMessage<EchoRequest>()`
- `DecodeMessage<EchoRequest>()`
- `EncodeMessage<AddRequest>()`
- `DecodeMessage<AddRequest>()`
- `EncodeMessage<SleepRequest>()`
- `DecodeMessage<SleepRequest>()`

保留断言语义不变。

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_codec_test`

Expected:

- FAIL，当前模板接口尚未提供

**Step 3: Write minimal implementation**

不在此任务实现生产代码。

**Step 4: Run test to verify it still fails for the missing template API**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_codec_test`

Expected:

- FAIL，报模板接口不存在或调用不匹配

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_codec_test.cpp
git commit -m "test: switch minirpc codec tests to template api"
```

### Task 2: 实现模板 codec traits

**Files:**
- Modify: `include/apps/minirpc/common/minirpc_codec.h`
- Modify: `src/apps/minirpc/common/minirpc_codec.cpp`

**Step 1: Write the failing test**

使用 Task 1 已改好的测试作为失败测试。

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_codec_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

在头文件中增加：

- `template <typename T> struct CodecTraits;`
- `template <typename T> bool EncodeMessage(...)`
- `template <typename T> bool DecodeMessage(...)`
- `EchoRequest`
- `EchoReply`
- `AddRequest`
- `AddReply`
- `SleepRequest`
- `SleepReply`

对应的 `CodecTraits<T>` 显式特化。

删除旧的 `EncodeXxx/DecodeXxx` 声明与定义。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_codec_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_codec.h src/apps/minirpc/common/minirpc_codec.cpp tests/apps/minirpc/minirpc_codec_test.cpp
git commit -m "refactor: template minirpc codec api"
```

### Task 3: 迁移 async/sync client 调用点

**Files:**
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_client.cpp`

**Step 1: Write the failing test**

使用现有：

- `tests/apps/minirpc/minirpc_client_test.cpp`

作为回归测试。

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_minirpc_client_test`

Expected:

- FAIL，旧 codec 函数名已删除但调用点尚未迁移

**Step 3: Write minimal implementation**

将：

- `EncodeEchoRequest()` 改为 `EncodeMessage<EchoRequest>()`
- `EncodeAddRequest()` 改为 `EncodeMessage<AddRequest>()`
- `EncodeSleepRequest()` 改为 `EncodeMessage<SleepRequest>()`
- `DecodeEchoReply()` 改为 `DecodeMessage<EchoReply>()`
- `DecodeAddReply()` 改为 `DecodeMessage<AddReply>()`
- `DecodeSleepReply()` 改为 `DecodeMessage<SleepReply>()`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_client_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/apps/minirpc/parent/minirpc_async_client.cpp src/apps/minirpc/parent/minirpc_client.cpp
git commit -m "refactor: use templated minirpc codec in clients"
```

### Task 4: 完整验证模板 codec 迁移

**Files:**
- Modify: `include/apps/minirpc/common/minirpc_codec.h`
- Modify: `src/apps/minirpc/common/minirpc_codec.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_client.cpp`
- Modify: `tests/apps/minirpc/minirpc_codec_test.cpp`

**Step 1: Write the failing test**

不新增测试，复用前面已切换的测试集。

**Step 2: Run test to verify current state**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R "memrpc_minirpc_(headers|codec|service|client)_test"`

Expected:

- 全部 PASS

**Step 3: Write minimal implementation**

仅在必要时修正模板可见性、头文件依赖或调用点细节，不额外扩展行为。

**Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R "memrpc_minirpc_(headers|codec|service|client)_test"`

Expected:

- 全部 PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_codec.h src/apps/minirpc/common/minirpc_codec.cpp src/apps/minirpc/parent/minirpc_async_client.cpp src/apps/minirpc/parent/minirpc_client.cpp tests/apps/minirpc/minirpc_codec_test.cpp
git commit -m "refactor: migrate minirpc codec to template traits"
```
