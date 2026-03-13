# MiniRpc View Decode + Throughput Baseline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 MiniRpc 全量补齐 view decode 路径，并新增吞吐量基线测试，支持“首次生成、后续上调、回退>10%失败”。

**Architecture:** 在 `minirpc_types`/`minirpc_codec` 增加 view 类型与解码，服务端默认使用 view decode，同时保留 owning decode。新增 GTest 吞吐量测试，记录基线并对比。

**Tech Stack:** C++17、GTest、shared memory、`eventfd`、`std::string_view`

---

### Task 1: 扩展 MiniRpc view 类型与解码

**Files:**
- Modify: `include/apps/minirpc/common/minirpc_types.h`
- Modify: `include/apps/minirpc/common/minirpc_codec.h`
- Modify: `tests/apps/minirpc/minirpc_codec_test.cpp`

**Step 1: Write the failing test**

在 `minirpc_codec_test.cpp` 中新增 view decode 覆盖：

```cpp
TEST(MiniRpcCodecTest, AllViewsDecodeRoundTrip) {
  EchoRequest request{"hello"};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeMessage<EchoRequest>(request, &bytes));
  EchoRequestView view_request;
  ASSERT_TRUE(DecodeMessageView<EchoRequestView>(bytes, &view_request));
  EXPECT_EQ(view_request.text, "hello");

  AddRequest add{1, 2};
  ASSERT_TRUE(EncodeMessage<AddRequest>(add, &bytes));
  AddRequestView add_view;
  ASSERT_TRUE(DecodeMessageView<AddRequestView>(bytes, &add_view));
  EXPECT_EQ(add_view.lhs, 1);
  EXPECT_EQ(add_view.rhs, 2);

  SleepReply sleep_reply{0};
  ASSERT_TRUE(EncodeMessage<SleepReply>(sleep_reply, &bytes));
  SleepReplyView sleep_view;
  ASSERT_TRUE(DecodeMessageView<SleepReplyView>(bytes, &sleep_view));
  EXPECT_EQ(sleep_view.status, 0);
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_codec_test'`

Expected: FAIL，因为尚无 `*View` 类型与 `ViewTraits`。

**Step 3: Write minimal implementation**

在 `minirpc_types.h` 增加 view 类型：

```cpp
struct EchoReplyView { std::string_view text; };
struct AddRequestView { int32_t lhs = 0; int32_t rhs = 0; };
struct AddReplyView { int32_t sum = 0; };
struct SleepRequestView { uint32_t delay_ms = 0; };
struct SleepReplyView { int32_t status = 0; };
```

在 `minirpc_codec.h` 增加 `ViewTraits`：

```cpp
template <>
struct ViewTraits<AddRequestView> {
  static bool Decode(const uint8_t* bytes, std::size_t size, AddRequestView* request) {
    if (request == nullptr) return false;
    ::memrpc::ByteReader reader(bytes, size);
    return reader.ReadInt32(&request->lhs) && reader.ReadInt32(&request->rhs);
  }
};
```

其余 view 类型按字段读取即可。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_codec_test'`

Expected: PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/common/minirpc_types.h include/apps/minirpc/common/minirpc_codec.h tests/apps/minirpc/minirpc_codec_test.cpp
git commit -m "feat: add minirpc view decode types"
```

---

### Task 2: MiniRpcService 支持 view/owning 双模式

**Files:**
- Modify: `include/apps/minirpc/child/minirpc_service.h`
- Modify: `src/apps/minirpc/child/minirpc_service.cpp`
- Modify: `tests/apps/minirpc/minirpc_client_test.cpp`

**Step 1: Write the failing test**

在 `minirpc_client_test.cpp` 中新增：

```cpp
TEST(MiniRpcClientTest, OwningDecodeHandlersStillWork) {
  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), MemRpc::StatusCode::Ok);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  MiniRpcService service;
  service.RegisterHandlers(&server, MiniRpcService::DecodeMode::Owning);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  AddReply reply;
  EXPECT_EQ(client.Add(1, 2, &reply), MemRpc::StatusCode::Ok);
  EXPECT_EQ(reply.sum, 3);

  client.Shutdown();
  server.Stop();
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_client_test'`

Expected: FAIL，因为 `RegisterHandlers` 尚无 `DecodeMode` 选择。

**Step 3: Write minimal implementation**

在 `minirpc_service.h` 中新增：

```cpp
enum class DecodeMode { Owning, View };
void RegisterHandlers(MemRpc::RpcServer* server, DecodeMode mode = DecodeMode::View);
```

在 `minirpc_service.cpp` 中根据 `mode` 切换 `DecodeMessage`/`DecodeMessageView`。

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_client_test'`

Expected: PASS

**Step 5: Commit**

```bash
git add include/apps/minirpc/child/minirpc_service.h src/apps/minirpc/child/minirpc_service.cpp tests/apps/minirpc/minirpc_client_test.cpp
git commit -m "feat: allow minirpc handlers to choose view or owning decode"
```

---

### Task 3: 吞吐量基线测试（owning vs view）

**Files:**
- Create: `tests/apps/minirpc/minirpc_throughput_test.cpp`
- Modify: `tests/apps/minirpc/CMakeLists.txt`

**Step 1: Write the failing test**

新增吞吐测试骨架，包含：

```cpp
TEST(MiniRpcThroughputTest, RecordsAndValidatesBaseline) {
  // 启动 server + client
  // 跑 Echo/Add/Sleep(0) 在 view/owning 两种模式下的 ops/sec
  // 读取/写入基线文件
  // 低于基线 10% 失败
}
```

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_throughput_test'`

Expected: FAIL，因为测试尚未注册到构建系统，且基线逻辑未实现。

**Step 3: Write minimal implementation**

实现要点：

- 新增测试到 `tests/apps/minirpc/CMakeLists.txt`：
  ```cmake
  minirpc_add_test(memrpc_minirpc_throughput_test minirpc_throughput_test.cpp)
  ```
- 基线文件默认路径：`build/perf_baselines/minirpc_throughput.baseline`
- key 格式：`<rpc>.<mode>.threads=<N>`，value 为 `ops_per_sec`
- 首次无基线：写入并 PASS
- 有基线：低于 10% FAIL，高于基线则更新为更高值
- 环境变量：
  - `MEMRPC_PERF_THREADS`
  - `MEMRPC_PERF_durationMs`
  - `MEMRPC_PERF_WARMUP_MS`
  - `MEMRPC_PERF_BASELINE_PATH`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R 'memrpc_minirpc_throughput_test'`

Expected: PASS（首次生成基线）。

**Step 5: Commit**

```bash
git add tests/apps/minirpc/minirpc_throughput_test.cpp tests/apps/minirpc/CMakeLists.txt
git commit -m "test: add minirpc throughput baseline comparison"
```

---

### Task 4: 文档补充（可选，但推荐）

**Files:**
- Modify: `docs/architecture.md`

**Step 1: Write the failing test**

（无强制测试）

**Step 2: Write minimal implementation**

补充 `MiniRpc` view/owning 双模式及吞吐基线策略的简要说明。

**Step 3: Commit**

```bash
git add docs/architecture.md
git commit -m "docs: note minirpc throughput baseline strategy"
```
