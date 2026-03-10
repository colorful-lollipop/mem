# Small Payload Copy Reduction Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 以“小 payload 高频 RPC”为目标，减少 `memrpc` 请求/回包全链路中的整块内存拷贝，先去掉纯浪费拷贝，再把热点业务编解码改成 view-based，避免无意义的对象 materialize。

**Architecture:** Phase 1 只做低风险拷贝削减，不改变共享内存协议，重点是把 `RpcCall`、`RpcReply`、VPS 封装中的多余 `std::vector<uint8_t>` 复制改成 move 或直接写入。Phase 2 在此基础上引入 byte-view 解码路径，让 server handler 和 client 解码逻辑尽量直接消费 `PayloadView` / 字节视图，而不是先把请求或回包重建成 owning buffer。

**Tech Stack:** C++17、CMake、GTest、shared memory、eventfd、`std::vector<uint8_t>`、view-based codec

---

## 当前基线

- `RpcClient::InvokeAsync(const RpcCall&)` 会把整份 `RpcCall` 复制进提交队列，`payload` 至少多拷一次
- submitter 把 `payload` 再 `memcpy` 到 request slot
- response writer 把 `reply.payload` 再 `memcpy` 到 response slot
- client 收到回包后把 response slot 复制到 `RpcReply.payload`
- `RpcFuture::Wait()` 再把内部 `RpcReply` 复制给调用方
- `MiniRpc` 和 `VPS` 业务层仍然普遍采用 “对象 -> `vector` -> decode 成对象” 的 owning codec
- `VPS` 里还有 envelope 级的中间 `vector`，例如 `result_bytes` / `event_bytes`

## 设计约束

- 不改变 request/response shared memory layout
- 不改变 `Reply/Event` 共用响应队列的协议
- 优先优化小包高频固定成本，不为追求零拷贝引入复杂生命周期陷阱
- Phase 1 完成前，不引入“直接向 request slot 编码”的大改
- Phase 2 的 view 类型只覆盖热点路径，保留 owning codec 作为兼容退路

### Task 1: 建立请求/回包拷贝基线测试

**Files:**
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_client_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_codec_test.cpp`
- Modify: `src/core/session_test_hook.h`

**Step 1: Write the failing test**

补测试覆盖：

- `InvokeAsync()` 支持 move-only 提交流程的 API 护栏
- `RpcFuture` 支持把回包 move 给调用方，而不是只能 copy
- `MiniRpc` 小请求 round-trip 走新 API 后仍然稳定
- 针对小 payload，测试能够区分 “copy path” 和 “move path”

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test|minirpc_client_test|minirpc_codec_test'`

Expected:

- FAIL，因为当前 `RpcClient` 只有 `const RpcCall&` 路径，`RpcFuture` 也只有 copy-out 语义

**Step 3: Write minimal implementation**

最小实现：

- 先补 API 和测试护栏，不改主逻辑
- 测试明确要求 move 路径可编译、可调用、可保持结果正确

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test|minirpc_client_test|minirpc_codec_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp tests/apps/minirpc/minirpc_client_test.cpp tests/apps/minirpc/minirpc_codec_test.cpp src/core/session_test_hook.h
git commit -m "test: add rpc copy-reduction api guards"
```

### Task 2: 给 `RpcClient` 和 `RpcFuture` 增加 move-aware 路径

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `RpcClient::InvokeAsync(RpcCall&&)` 存在并优先走 move 语义
- `PendingSubmit` 能接收 moved `payload`
- `RpcFuture` 提供 `WaitAndTake()` 或等价 move-out 接口
- 新接口下结果码和 payload 仍正确

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，因为当前 `submit.call = call` 会复制整份 payload，且 `Wait()` 只支持 copy-out

**Step 3: Write minimal implementation**

最小实现：

- 在 `RpcClient` public API 中新增 `InvokeAsync(RpcCall&&)`
- `Impl::InvokeAsync()` 接受按值或 rvalue 的 `RpcCall`，并 `std::move` 进入 `PendingSubmit`
- `RpcFuture` 增加 move-out 回包接口，内部 `state_->reply` 改为可移动交付
- 保持现有 `InvokeAsync(const RpcCall&)` / `Wait()` 兼容

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_rpc_client_api_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: add move-aware rpc request and reply paths"
```

### Task 3: 把 `MiniRpc` 和 `VPS` 调用侧改成 move 提交

**Files:**
- Modify: `src/apps/minirpc/parent/minirpc_async_client.cpp`
- Modify: `src/apps/minirpc/parent/minirpc_client.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`
- Modify: `tests/apps/minirpc/minirpc_client_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `MiniRpcAsyncClient` 编码后直接 move 进 `RpcCall`
- `VirusEngineManager::MakeCall()` 和调用点不再对 lvalue `requestBytes` 做额外复制
- 现有同步和异步 round-trip 都保持行为不变

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'minirpc_client_test|memrpc_rpc_client_integration_test'`

Expected:

- FAIL，直到调用封装统一切到 move 语义

**Step 3: Write minimal implementation**

最小实现：

- `MiniRpc` 继续先编码到 `payload`，但编码完成后只做 move，不做额外 copy
- `VirusEngineManager::MakeCall()` 改为只接受 rvalue 或按值后强制 move 使用
- 所有热点 call site 统一用 `std::move(requestBytes)` 构造请求

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'minirpc_client_test|memrpc_rpc_client_integration_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/apps/minirpc/parent/minirpc_async_client.cpp src/apps/minirpc/parent/minirpc_client.cpp src/apps/vps/parent/virus_engine_manager.cpp tests/apps/minirpc/minirpc_client_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "perf: move encoded payloads through app call sites"
```

### Task 4: 消除 VPS codec 的中间 envelope `vector`

**Files:**
- Modify: `include/apps/vps/common/vps_codec.h`
- Modify: `src/apps/vps/common/vps_codec.cpp`
- Create: `tests/apps/vps/vps_codec_test.cpp`
- Modify: `tests/apps/CMakeLists.txt`

**Step 1: Write the failing test**

补测试覆盖：

- `EncodeScanFileReply()` 不再通过中间 `result_bytes` 组包
- `EncodePollBehaviorEventReply()` 不再通过中间 `event_bytes` 组包
- `DecodeScanFileReply()` / `DecodePollBehaviorEventReply()` 不再切新的子 `vector`
- 编码结果与现有协议字节兼容

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'vps_codec_test'`

Expected:

- FAIL，因为当前实现仍会构造中间 `vector`

**Step 3: Write minimal implementation**

最小实现：

- 增加 `Encode...ToWriter()` / `Decode...FromReader()` 内部辅助函数
- 外层 reply/event 直接写同一个 `ByteWriter`
- decode 直接在原始字节流上推进 offset，不再 materialize 子 buffer

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'vps_codec_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/vps/common/vps_codec.h src/apps/vps/common/vps_codec.cpp tests/apps/vps/vps_codec_test.cpp tests/apps/CMakeLists.txt
git commit -m "perf: remove nested vps codec buffer copies"
```

### Task 5: 引入 byte-view 解码基础设施

**Files:**
- Modify: `src/core/byte_reader.h`
- Modify: `src/core/byte_reader.cpp`
- Modify: `tests/memrpc/byte_codec_test.cpp`
- Modify: `include/memrpc/server/handler.h`

**Step 1: Write the failing test**

补测试覆盖：

- `ByteReader` 能读出 `string_view` 风格字段，而不是只能 `assign` 到 `std::string`
- 对 `uint32/int32/bytes/string` 同时支持 owning decode 和 view decode
- `PayloadView` 可以无拷贝地喂给新的 reader/view codec

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_byte_codec_test'`

Expected:

- FAIL，因为当前 `ByteReader` 只有 copy-out API

**Step 3: Write minimal implementation**

最小实现：

- 在 `ByteReader` 中新增只读 view API，例如 `ReadStringView()` / `ReadBytesView()`
- 保持现有 `ReadString()` / `ReadBytes()` 兼容
- 如有必要，在 `handler.h` 中补充轻量 view 类型别名

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_byte_codec_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/byte_reader.h src/core/byte_reader.cpp tests/memrpc/byte_codec_test.cpp include/memrpc/server/handler.h
git commit -m "feat: add byte reader view decoding"
```

### Task 6: 让 `MiniRpc` 先走 view-based request decode

**Files:**
- Modify: `include/apps/minirpc/common/minirpc_codec.h`
- Modify: `include/apps/minirpc/common/minirpc_types.h`
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `tests/apps/minirpc/minirpc_codec_test.cpp`
- Modify: `tests/apps/minirpc/minirpc_service_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `EchoRequest` 提供 view 版 decode，handler 可直接读取 `string_view`
- `AddRequest` / `SleepRequest` 继续支持 trivially-copyable 小对象路径
- 旧 owning decode API 保持兼容

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'minirpc_codec_test|minirpc_service_test'`

Expected:

- FAIL，因为当前 `MiniRpcService` 仍先 decode 成 owning request

**Step 3: Write minimal implementation**

最小实现：

- 在 `CodecTraits` 旁新增 `DecodeView()` 或 `ViewTraits`
- `Echo` handler 直接使用 view 请求，不再构造中间 `EchoRequest`
- `Add` / `Sleep` 只在确有收益时调整，不做无谓复杂化

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'minirpc_codec_test|minirpc_service_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_codec.h include/apps/minirpc/common/minirpc_types.h src/apps/minirpc/child/minirpc_service.cpp tests/apps/minirpc/minirpc_codec_test.cpp tests/apps/minirpc/minirpc_service_test.cpp
git commit -m "feat: add minirpc request view decoding"
```

### Task 7: 给 VPS 热点请求和回包增加 view-based codec

**Files:**
- Modify: `include/apps/vps/common/vps_codec.h`
- Modify: `src/apps/vps/common/vps_codec.cpp`
- Modify: `src/apps/vps/child/virus_engine_service.cpp`
- Modify: `src/apps/vps/parent/virus_engine_manager.cpp`
- Modify: `tests/apps/vps/vps_codec_test.cpp`

**Step 1: Write the failing test**

补测试覆盖：

- `ScanBehaviorRequest`、`AccessTokenRequest` 这类小包请求可直接从 payload view decode
- `ScanFileReply` 外层 envelope 可直接在原始回包上解析，不切子 `vector`
- 热点 decode 路径继续与旧协议字节兼容

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'vps_codec_test'`

Expected:

- FAIL，因为当前 VPS codec 仍默认构造成 owning object 或中间 buffer

**Step 3: Write minimal implementation**

最小实现：

- 为 VPS codec 增加 view 版 decode 函数
- `VirusEngineService` 优先消费 view 型请求
- `VirusEngineManager` 对简单回包优先用 view/decode-on-reader 路径

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'vps_codec_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/apps/vps/common/vps_codec.h src/apps/vps/common/vps_codec.cpp src/apps/vps/child/virus_engine_service.cpp src/apps/vps/parent/virus_engine_manager.cpp tests/apps/vps/vps_codec_test.cpp
git commit -m "feat: add vps view-based codec paths"
```

### Task 8: 文档化新拷贝语义并补回归验证

**Files:**
- Modify: `docs/architecture.md`
- Modify: `docs/demo_guide.md`
- Modify: `tests/memrpc/build_config_test.cpp`

**Step 1: Write the failing test**

补验证覆盖：

- 文档和测试明确：请求路径已支持 move-aware 提交
- server handler 可直接消费 payload view
- owning codec 与 view codec 的边界清晰，不要求所有业务一次性迁完

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test'`

Expected:

- FAIL，直到文档引用和测试清单更新完成

**Step 3: Write minimal implementation**

最小实现：

- 更新架构文档中的请求/回包拷贝说明
- 记录 Phase 1 / Phase 2 的适用边界
- 保证新增 VPS 测试进入主构建

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_build_config_test'`

Expected:

- PASS

**Step 5: Commit**

```bash
git add docs/architecture.md docs/demo_guide.md tests/memrpc/build_config_test.cpp
git commit -m "docs: describe reduced-copy rpc data flow"
```
