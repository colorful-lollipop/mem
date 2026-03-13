# TODO: Server Executor Decoupling Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Decouple `RpcServer` request execution from the built-in worker pool by introducing a `TaskExecutor` interface with default thread-pool behavior, while preserving current backpressure semantics.

**Architecture:** Add a small `TaskExecutor` interface in `memrpc/core`, update `ServerOptions` to accept injected executors, and swap the internal `WorkerPool` usage in `RpcServer` for `TaskExecutor` instances. Provide a default `ThreadPoolExecutor` that mirrors current behavior. Add tests to validate custom executor gating.

**Tech Stack:** C++17, CMake, GoogleTest.

---

### Task 1: Add a failing test for custom executor gating

**Files:**
- Create: `tests/memrpc/rpc_server_executor_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "memrpc/client/rpc_client.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/core/task_executor.h"
#include "memrpc/server/rpc_server.h"

constexpr memrpc::Opcode kTestOpcode = 1u;

namespace {

class TestExecutor final : public memrpc::TaskExecutor {
 public:
  explicit TestExecutor(uint32_t max_inflight) : max_inflight_(max_inflight) {}

  void SetMaxInflight(uint32_t max_inflight) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_inflight_ = max_inflight;
    cv_.notify_all();
  }

  bool TrySubmit(std::function<void()> task) override {
    if (!task) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_ || inflight_ >= max_inflight_) {
        return false;
      }
      ++inflight_;
    }
    task();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (inflight_ > 0) {
        --inflight_;
      }
      cv_.notify_all();
    }
    return true;
  }

  bool HasCapacity() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopped_ && inflight_ < max_inflight_;
  }

  bool WaitForCapacity(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
      return stopped_ || inflight_ < max_inflight_;
    });
  }

  void Stop() override {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  uint32_t max_inflight_ = 0;
  uint32_t inflight_ = 0;
  bool stopped_ = false;
};

bool WaitForCondition(const std::function<bool()>& condition, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return condition();
}

}  // namespace

TEST(RpcServerExecutorTest, CustomExecutorGatesDrain) {
  auto bootstrap = std::make_shared<memrpc::SaBootstrapChannel>();
  memrpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(&unused_handles), memrpc::StatusCode::Ok);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->server_handles());

  auto executor = std::make_shared<TestExecutor>(0);
  memrpc::ServerOptions options;
  options.high_executor = executor;
  options.normal_executor = executor;
  server.SetOptions(options);
  server.RegisterHandler(kTestOpcode,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           ASSERT_NE(reply, nullptr);
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  memrpc::RpcCall call;
  call.opcode = kTestOpcode;
  call.payload = std::vector<uint8_t>{1, 2, 3};

  auto future = client.InvokeAsync(call);
  EXPECT_FALSE(WaitForCondition([&future] { return future.IsReady(); }, 20));

  executor->SetMaxInflight(1);
  memrpc::RpcReply reply;
  EXPECT_EQ(future.Wait(&reply), memrpc::StatusCode::Ok);
  EXPECT_EQ(reply.payload, call.payload);

  client.Shutdown();
  server.Stop();
}
```

Add the test target:

```cmake
memrpc_add_test(memrpc_rpc_server_executor_test rpc_server_executor_test.cpp)
```

**Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: build fails because `memrpc::TaskExecutor` and `ServerOptions::high_executor/normal_executor` do not exist yet.

**Step 3: Commit the failing test**

```bash
git add tests/memrpc/rpc_server_executor_test.cpp tests/memrpc/CMakeLists.txt
git commit -m "test: add custom executor gating test"
```

---

### Task 2: Introduce `TaskExecutor` and extend `ServerOptions`

**Files:**
- Create: `include/memrpc/core/task_executor.h`
- Modify: `include/memrpc/server/rpc_server.h`

**Step 1: Write minimal interface header**

```cpp
#ifndef MEMRPC_CORE_TASK_EXECUTOR_H_
#define MEMRPC_CORE_TASK_EXECUTOR_H_

#include <chrono>
#include <functional>

namespace memrpc {

class TaskExecutor {
 public:
  virtual ~TaskExecutor() = default;

  // Submit returns false if the executor cannot accept new work.
  virtual bool TrySubmit(std::function<void()> task) = 0;
  // HasCapacity + TrySubmit must be consistent for a single producer.
  virtual bool HasCapacity() const = 0;
  virtual bool WaitForCapacity(std::chrono::milliseconds timeout) = 0;
  virtual void Stop() = 0;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_TASK_EXECUTOR_H_
```

**Step 2: Extend `ServerOptions`**

```cpp
namespace memrpc {

class TaskExecutor;

struct ServerOptions {
  uint32_t high_worker_threads = 1;
  uint32_t normal_worker_threads = 1;
  uint32_t completion_queue_capacity = 0;
  std::shared_ptr<TaskExecutor> high_executor;
  std::shared_ptr<TaskExecutor> normal_executor;
};
```

**Step 3: Build to ensure compile progresses**

Run: `cmake --build build`
Expected: build succeeds or fails later in `rpc_server.cpp` because executor fields are unused.

**Step 4: Commit**

```bash
git add include/memrpc/core/task_executor.h include/memrpc/server/rpc_server.h
git commit -m "feat: add task executor interface and options"
```

---

### Task 3: Replace `WorkerPool` usage with `TaskExecutor`

**Files:**
- Modify: `src/server/rpc_server.cpp`

**Step 1: Implement default `ThreadPoolExecutor` inside `rpc_server.cpp`**

```cpp
class ThreadPoolExecutor final : public TaskExecutor {
 public:
  explicit ThreadPoolExecutor(uint32_t threadCount) {
    const uint32_t threads = std::max(1u, threadCount);
    queue_capacity_ = threads;
    running_ = true;
    for (uint32_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  ~ThreadPoolExecutor() override {
    Stop();
  }

  bool TrySubmit(std::function<void()> task) override {
    if (!task) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || queue_.size() >= queue_capacity_) {
        return false;
      }
      queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  bool HasCapacity() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && queue_.size() < queue_capacity_;
  }

  bool WaitForCapacity(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
      return !running_ || queue_.size() < queue_capacity_;
    });
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

 private:
  void WorkerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop();
        cv_.notify_one();
      }
      task();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool running_ = false;
  uint32_t queue_capacity_ = 1;
};
```

**Step 2: Replace `WorkerPool` with `TaskExecutor` in `Impl`**
- Store `std::shared_ptr<TaskExecutor> high_executor;` and `normal_executor;`
- In `Start()`, set executors from options or create `ThreadPoolExecutor`
- In `Stop()`, call `Stop()` on executors and reset
- Update `DrainQueue()` and `HandleBackloggedQueues()` to use executors

**Step 3: Run the new test**

Run: `ctest --test-dir build -R memrpc_rpc_server_executor_test --output-on-failure`
Expected: PASS

**Step 4: Commit**

```bash
git add src/server/rpc_server.cpp
git commit -m "feat: decouple rpc server execution with task executor"
```

---

### Task 4: Verify broader test coverage

**Files:**
- None

**Step 1: Run affected test subset**

Run: `ctest --test-dir build -R memrpc_rpc_server_api_test --output-on-failure`
Expected: PASS

**Step 2: Run full tests (optional)**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS

**Step 3: Commit if any fixes were needed**

```bash
git add -A
git commit -m "fix: address executor integration test fallout"
```
