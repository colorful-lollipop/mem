# 通用 RPC 槽位化改造 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将当前写死在 `Scan` 上的共享内存调用层抽成通用 RPC 内核，并先完成 `Scan` 迁移，为后续 `ScanBehavior` 等函数提供最小改动扩展点。

**Architecture:** 保留现有共享内存、双优先级队列、双线程池和恢复逻辑，只把 slot payload、client 调用入口和 server 分发层改造成通用 request/response 字节区模型。第一阶段不新增实际业务 RPC，只迁移 `Scan` 并保持行为不变。

**Tech Stack:** C++17、POSIX shared memory、eventfd、GTest、CMake、GN

---

### Task 1: 为通用协议增加失败测试

**Files:**
- Modify: `tests/protocol_layout_test.cpp`
- Test: `tests/protocol_layout_test.cpp`

**Step 1: Write the failing test**

新增测试，断言：

- `Opcode::ScanFile` 仍然存在
- `SlotPayload` 含通用 request/response 头时大小大于当前版本
- 新增的默认常量为“请求 `16 * 1024`、响应 `1024`”
- session 布局配置支持统一的 `max_request_bytes`、`max_response_bytes`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_protocol_layout_test`

Expected:

- FAIL，提示新常量或新字段不存在

**Step 3: Write minimal implementation**

修改：

- `src/core/protocol.h`

增加：

- `kDefaultMaxRequestBytes`
- `kDefaultMaxResponseBytes`
- `RpcRequestHeader`
- `RpcResponseHeader`
- 通用 `SlotPayload`
- session 统一请求/响应大小配置

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_protocol_layout_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h tests/protocol_layout_test.cpp
git commit -m "refactor: add generic rpc slot payload"
```

### Task 2: 为简单二进制编解码工具增加失败测试

**Files:**
- Create: `tests/byte_codec_test.cpp`
- Create: `src/core/byte_reader.h`
- Create: `src/core/byte_reader.cpp`
- Create: `src/core/byte_writer.h`
- Create: `src/core/byte_writer.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `BUILD.gn`

**Step 1: Write the failing test**

新增测试覆盖：

- 写入并读回 `uint32_t`
- 写入并读回 `int32_t`
- 写入并读回 `std::string`
- 越界读取失败

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_byte_codec_test`

Expected:

- FAIL，目标不存在或符号不存在

**Step 3: Write minimal implementation**

实现最小接口：

- `ByteWriter::WriteUint32`
- `ByteWriter::WriteInt32`
- `ByteWriter::WriteBytes`
- `ByteWriter::WriteString`
- `ByteReader::ReadUint32`
- `ByteReader::ReadInt32`
- `ByteReader::ReadBytes`
- `ByteReader::ReadString`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_byte_codec_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/byte_reader.* src/core/byte_writer.* tests/byte_codec_test.cpp src/CMakeLists.txt tests/CMakeLists.txt BUILD.gn
git commit -m "feat: add minimal byte codec helpers"
```

### Task 3: 为 `Scan` codec 增加失败测试

**Files:**
- Create: `tests/scan_codec_test.cpp`
- Create: `src/rpc/scan_codec.h`
- Create: `src/rpc/scan_codec.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `BUILD.gn`

**Step 1: Write the failing test**

新增测试覆盖：

- `ScanRequest.file_path` 编码后可正确解码
- `ScanResult` 编码后可正确解码
- 超过上限的字符串返回失败

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_scan_codec_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

实现：

- `EncodeScanRequest`
- `DecodeScanRequest`
- `EncodeScanResult`
- `DecodeScanResult`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_scan_codec_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/rpc/scan_codec.* tests/scan_codec_test.cpp src/CMakeLists.txt tests/CMakeLists.txt BUILD.gn
git commit -m "feat: add scan rpc codec"
```

### Task 4: 为客户端通用 `Invoke()` 增加失败测试

**Files:**
- Modify: `tests/integration_end_to_end_test.cpp`
- Modify: `src/client/engine_client.cpp`

**Step 1: Write the failing test**

新增或调整测试，要求：

- `Scan()` 仍能正常返回 `Clean`
- 高优请求仍能优先处理
- 请求编码后通过通用 payload 路径返回结果

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- FAIL，说明老的专用 payload 逻辑与新测试不匹配

**Step 3: Write minimal implementation**

在 `src/client/engine_client.cpp`：

- 新增内部 `RpcCall` / `RpcReply`
- 新增 `Invoke()`
- 让 `Scan()` 仅负责调用 `scan_codec`
- 保持死亡回调、恢复和双优先级逻辑不变

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/client/engine_client.cpp tests/integration_end_to_end_test.cpp
git commit -m "refactor: route scan through generic client invoke"
```

### Task 5: 为服务端通用 handler 注册表增加失败测试

**Files:**
- Modify: `tests/integration_end_to_end_test.cpp`
- Modify: `include/memrpc/handler.h`
- Modify: `src/server/engine_server.cpp`

**Step 1: Write the failing test**

新增测试覆盖：

- `Scan` 通过 `opcode` 注册表被正确分发
- 未注册 `opcode` 返回 `InvalidArgument`

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

修改：

- `include/memrpc/handler.h`
- `src/server/engine_server.cpp`

实现：

- `RpcHandler` 定义
- `opcode -> handler` 注册表
- `Scan` handler 注册
- 未知 `opcode` 的错误返回

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_integration_end_to_end_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/handler.h src/server/engine_server.cpp tests/integration_end_to_end_test.cpp
git commit -m "refactor: add generic server rpc dispatch"
```

### Task 6: 更新文档和 demo 验证通用 `Scan` 路径

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `docs/porting_guide.md`
- Modify: `demo/demo_dual_process_main.cpp`

**Step 1: Write the failing test**

这里不新增单元测试，直接把行为验证落到现有 demo 和集成测试。

**Step 2: Run verification to establish baseline**

Run: `./build/demo/memrpc_demo_dual_process`

Expected:

- 现有 demo 正常输出

**Step 3: Write minimal implementation**

更新：

- 中文文档说明当前框架已经从专用 `Scan` payload 升级为通用 RPC 槽位
- demo 仍以 `Scan` 为例，不额外增加复杂示例

**Step 4: Run full verification**

Run:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/demo/memrpc_demo_dual_process`

Expected:

- 全部通过

**Step 5: Commit**

```bash
git add docs/architecture.md docs/demo_guide.md docs/porting_guide.md demo/demo_dual_process_main.cpp
git commit -m "docs: document generic rpc scan path"
```

### Task 7: 为第二阶段 `ScanBehavior` 预留实施入口

**Files:**
- Modify: `src/core/protocol.h`
- Optionally Modify: `include/memrpc/client.h`
- Optionally Modify: `include/memrpc/handler.h`

**Step 1: Write the failing test**

暂不为 `ScanBehavior` 写功能测试，只验证协议和扩展点存在。

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R memrpc_protocol_layout_test`

Expected:

- FAIL，若 `Opcode::ScanBehavior` 尚未定义

**Step 3: Write minimal implementation**

仅增加：

- `Opcode::ScanBehavior`

不实现业务逻辑。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_protocol_layout_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h tests/protocol_layout_test.cpp
git commit -m "chore: reserve scan behavior opcode"
```

### Task 8: 最终收口验证与提交

**Files:**
- Modify: `docs/plans/2026-03-09-generic-rpc-design.md`
- Modify: `docs/plans/2026-03-09-generic-rpc-implementation-plan.md`

**Step 1: Re-read changed files**

确认：

- 没有遗留旧的专用 `Scan` payload 字段
- `Scan` 已完全走通用 encode/invoke/decode 路径
- 服务端主流程不再写死 `HandleScan`

**Step 2: Run full verification**

Run:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/demo/memrpc_demo_dual_process`
- `git diff --stat`

Expected:

- 构建成功
- 全量测试通过
- demo 正常
- 变更范围与计划一致

**Step 3: Final commit**

```bash
git add .
git commit -m "refactor: generalize shared-memory rpc invocation"
```
