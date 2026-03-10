# Failure Callback Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 `memrpc::RpcClient` 增加框架级失败回调，并提供一个应用层失败监控器示例。

**Architecture:** 框架层新增 `RpcFailure` 结构体与 `RpcFailureCallback`，在提交阶段/响应阶段/会话失败时触发回调；应用层通过 `FailureMonitor` 聚合 `ExecTimeout` 并触发阈值回调。

**Tech Stack:** C++17, GoogleTest, CMake

---

### Task 1: 添加失败回调的测试（先让它失败）

**Files:**
- Modify: `tests/memrpc/rpc_client_api_test.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: 新增 API 层失败回调测试**

在 `tests/memrpc/rpc_client_api_test.cpp` 末尾新增测试：

```cpp
TEST(RpcClientApiTest, FailureCallbackFiresOnAdmissionFailure) {
  auto bootstrap = std::make_shared<FakeBootstrapChannel>();
  MemRpc::RpcClient client(bootstrap);

  std::mutex mutex;
  MemRpc::RpcFailure captured{};
  int calls = 0;
  client.SetFailureCallback([&](const MemRpc::RpcFailure& failure) {
    std::lock_guard<std::mutex> lock(mutex);
    captured = failure;
    ++calls;
  });

  MemRpc::RpcCall call;
  call.opcode = MemRpc::Opcode::ScanFile;
  call.priority = MemRpc::Priority::Normal;
  call.admission_timeout_ms = 1000;
  call.queue_timeout_ms = 0;
  call.exec_timeout_ms = 1000;

  auto future = client.InvokeAsync(call);
  MemRpc::RpcReply reply;
  const MemRpc::StatusCode status = future.Wait(&reply);

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(captured.status, status);
  EXPECT_EQ(captured.stage, MemRpc::FailureStage::Admission);
  EXPECT_EQ(captured.opcode, call.opcode);
  EXPECT_EQ(captured.priority, call.priority);
  EXPECT_EQ(captured.flags, call.flags);
  EXPECT_EQ(captured.admission_timeout_ms, call.admission_timeout_ms);
  EXPECT_EQ(captured.queue_timeout_ms, call.queue_timeout_ms);
  EXPECT_EQ(captured.exec_timeout_ms, call.exec_timeout_ms);
  EXPECT_NE(captured.request_id, 0u);
}
```

**Step 2: 新增集成层 ExecTimeout 回调测试**

在 `tests/memrpc/rpc_client_integration_test.cpp` 增加测试：

```cpp
TEST(RpcClientIntegrationTest, FailureCallbackFiresOnExecTimeout) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  ASSERT_EQ(bootstrap->StartEngine(), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());
  server.RegisterHandler(memrpc::Opcode::ScanFile,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           std::this_thread::sleep_for(std::chrono::milliseconds(5));
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  std::mutex mutex;
  memrpc::RpcFailure captured{};
  std::atomic<int> calls{0};
  client.SetFailureCallback([&](const memrpc::RpcFailure& failure) {
    if (failure.status == memrpc::StatusCode::ExecTimeout) {
      std::lock_guard<std::mutex> lock(mutex);
      captured = failure;
      calls.fetch_add(1);
    }
  });

  memrpc::RpcCall call;
  call.opcode = memrpc::Opcode::ScanFile;
  call.exec_timeout_ms = 1;
  call.queue_timeout_ms = 0;
  call.payload = std::vector<uint8_t>{1, 2, 3};

  memrpc::RpcReply reply;
  const memrpc::StatusCode status = client.InvokeAsync(call).Wait(&reply);
  EXPECT_EQ(status, memrpc::StatusCode::ExecTimeout);

  EXPECT_TRUE(WaitForCondition([&] { return calls.load() > 0; }, 200));
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(captured.status, memrpc::StatusCode::ExecTimeout);
    EXPECT_EQ(captured.stage, memrpc::FailureStage::Response);
    EXPECT_EQ(captured.opcode, call.opcode);
  }

  client.Shutdown();
  server.Stop();
}
```

**Step 3: 编译并确认失败**

Run: `cmake --build build`
Expected: 编译失败，报错类似 “`RpcFailure` 未声明” 或 “`FailureStage` 未声明”。

---

### Task 2: 实现框架级失败回调

**Files:**
- Modify: `include/memrpc/client/rpc_client.h`
- Modify: `src/client/rpc_client.cpp`

**Step 1: 在头文件中添加新类型与 API**

在 `include/memrpc/client/rpc_client.h` 增加：

```cpp
enum class FailureStage {
  Admission = 0,
  Response = 1,
  Session = 2,
};

struct RpcFailure {
  StatusCode status = StatusCode::Ok;
  Opcode opcode = Opcode::ScanFile;
  Priority priority = Priority::Normal;
  uint32_t flags = 0;
  uint32_t admission_timeout_ms = 0;
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint64_t request_id = 0;
  uint64_t session_id = 0;
  uint32_t monotonic_ms = 0;
  FailureStage stage = FailureStage::Admission;
};

using RpcFailureCallback = std::function<void(const RpcFailure&)>;
```

并在 `RpcClient` 类中新增：

```cpp
void SetFailureCallback(RpcFailureCallback callback);
```

**Step 2: 在实现中存储回调与元数据**

在 `src/client/rpc_client.cpp` 的 `RpcClient::Impl` 中新增：

```cpp
struct PendingInfo {
  Opcode opcode = Opcode::ScanFile;
  Priority priority = Priority::Normal;
  uint32_t flags = 0;
  uint32_t admission_timeout_ms = 0;
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint64_t request_id = 0;
  uint64_t session_id = 0;
};

std::vector<std::optional<PendingInfo>> pending_info;
std::mutex failure_mutex;
RpcFailureCallback failure_callback;
```

并新增辅助函数：

```cpp
void NotifyFailure(const PendingInfo& info, StatusCode status, FailureStage stage) {
  if (status == StatusCode::Ok) {
    return;
  }
  RpcFailureCallback callback;
  {
    std::lock_guard<std::mutex> lock(failure_mutex);
    callback = failure_callback;
  }
  if (!callback) {
    return;
  }
  RpcFailure failure;
  failure.status = status;
  failure.opcode = info.opcode;
  failure.priority = info.priority;
  failure.flags = info.flags;
  failure.admission_timeout_ms = info.admission_timeout_ms;
  failure.queue_timeout_ms = info.queue_timeout_ms;
  failure.exec_timeout_ms = info.exec_timeout_ms;
  failure.request_id = info.request_id;
  failure.session_id = info.session_id;
  failure.monotonic_ms = MonotonicNowMs();
  failure.stage = stage;
  callback(failure);
}
```

**Step 3: 补齐各失败路径回调触发**

- `RpcClient::SetFailureCallback` 实现：写入 `impl_->failure_callback`。
- `InvokeAsync` 中提前生成 `request_id`，如果 `EnsureLiveSession` 或 payload 校验失败，构造 `PendingInfo` 并调用 `NotifyFailure(..., FailureStage::Admission)`。
- `SubmitOne` 进入 slot 后写入 `pending_info[slot]`，失败时清理并触发 `NotifyFailure(..., FailureStage::Admission)`。
- `CompleteRequest` 在 `reply.status != Ok` 时，取出 `pending_info` 并触发 `NotifyFailure(..., FailureStage::Response)`。
- `FailQueuedSubmissions` / `FailAllPending` / `HandleEngineDeath`：对每个 pending 触发 `NotifyFailure(..., FailureStage::Session)`。

**Step 4: 重新编译并运行测试**

Run: `cmake --build build`
Expected: 编译成功

Run: `ctest --test-dir build --output-on-failure -R RpcClientApiTest`
Expected: PASS

Run: `ctest --test-dir build --output-on-failure -R RpcClientIntegrationTest`
Expected: PASS

**Step 5: 提交**

```bash
git add include/memrpc/client/rpc_client.h src/client/rpc_client.cpp \
  tests/memrpc/rpc_client_api_test.cpp tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: add rpc failure callback"
```

---

### Task 3: 添加应用层 FailureMonitor 示例

**Files:**
- Create: `include/apps/minirpc/parent/minirpc_failure_monitor.h`
- Modify: `tests/apps/minirpc/minirpc_dfx_test.cpp`

**Step 1: 先写测试**

在 `tests/apps/minirpc/minirpc_dfx_test.cpp` 增加测试：

```cpp
#include "apps/minirpc/parent/minirpc_failure_monitor.h"

TEST(MiniRpcDfxTest, FailureMonitorTriggersAfterExecTimeoutThreshold) {
  int triggered = 0;
  FailureMonitor::Options options;
  options.window_ms = 60000;
  options.exec_timeout_threshold = 3;

  FailureMonitor monitor(options, [&] { ++triggered; });

  MemRpc::RpcFailure failure;
  failure.status = MemRpc::StatusCode::ExecTimeout;

  monitor.OnFailure(failure);
  monitor.OnFailure(failure);
  EXPECT_EQ(triggered, 0);
  monitor.OnFailure(failure);
  EXPECT_EQ(triggered, 1);
}
```

**Step 2: 编译并确认失败**

Run: `cmake --build build`
Expected: 编译失败，提示找不到 `FailureMonitor` 定义。

**Step 3: 实现 `FailureMonitor` 头文件**

创建 `include/apps/minirpc/parent/minirpc_failure_monitor.h`：

```cpp
#ifndef APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_
#define APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>

#include "memrpc/client/rpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

class FailureMonitor {
 public:
  struct Options {
    uint32_t window_ms = 60000;
    uint32_t exec_timeout_threshold = 3;
    bool trigger_on_disconnect = true;
  };

  using ThresholdCallback = std::function<void()>;

  explicit FailureMonitor(Options options = {}, ThresholdCallback callback = nullptr)
      : options_(options), callback_(std::move(callback)) {}

  void SetThresholdCallback(ThresholdCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    exec_timeouts_.clear();
  }

  void OnFailure(const MemRpc::RpcFailure& failure) {
    ThresholdCallback callback;
    bool fire = false;
    if (failure.status == MemRpc::StatusCode::PeerDisconnected ||
        failure.status == MemRpc::StatusCode::ProtocolMismatch) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (options_.trigger_on_disconnect && callback_) {
        callback = callback_;
        fire = true;
      }
    } else if (failure.status == MemRpc::StatusCode::ExecTimeout) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(mutex_);
      exec_timeouts_.push_back(now);
      while (!exec_timeouts_.empty() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(now - exec_timeouts_.front()).count() >
                 options_.window_ms) {
        exec_timeouts_.pop_front();
      }
      if (exec_timeouts_.size() >= options_.exec_timeout_threshold && callback_) {
        callback = callback_;
        fire = true;
        exec_timeouts_.clear();
      }
    }
    if (fire && callback) {
      callback();
    }
  }

 private:
  Options options_;
  ThresholdCallback callback_;
  std::mutex mutex_;
  std::deque<std::chrono::steady_clock::time_point> exec_timeouts_;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_
```

**Step 4: 重新编译并运行测试**

Run: `cmake --build build`
Expected: 编译成功

Run: `ctest --test-dir build --output-on-failure -R memrpc_minirpc_dfx_test`
Expected: PASS

**Step 5: 提交**

```bash
git add include/apps/minirpc/parent/minirpc_failure_monitor.h \
  tests/apps/minirpc/minirpc_dfx_test.cpp
git commit -m "feat: add minirpc failure monitor"
```

---

### Task 4: 回归测试（可选但推荐）

**Files:**
- None

**Step 1: 跑核心测试集**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS

**Step 2: 提交（如有额外变更）**

```bash
git status -sb
```

