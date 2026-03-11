# Remove Test Hooks Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 移除 memrpc 的 test hook 代码及其依赖测试，保持主线逻辑无测试插桩。

**Architecture:** 删除 ring trace hook 与 fd 失败注入逻辑，删除依赖测试与 CMake 目标。主线行为不变，仅去掉测试可观察/注入路径。

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: 移除 ring trace hook 与依赖测试

**Files:**
- Delete: `src/core/session_test_hook.h`
- Modify: `src/core/session.cpp`
- Delete: `tests/memrpc/rpc_client_integration_test.cpp`
- Delete: `tests/memrpc/response_queue_event_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: 删除 test hook 头文件（制造编译失败）**

Run: `rm -f src/core/session_test_hook.h`

**Step 2: 编译确认失败**

Run: `cmake --build build`
Expected: 失败，提示找不到 `core/session_test_hook.h`（来自 `src/core/session.cpp` 或测试）。

**Step 3: 删除 hook 使用点与依赖测试**

`src/core/session.cpp`：
- 移除 `#include "core/session_test_hook.h"`。
- 删除 ring trace 相关的全局状态与辅助函数，以及 `SetRingTraceCallbackForTest` / `ClearRingTraceCallbackForTest` 的定义。
- `PushRingEntry` / `PopRingEntry` 不再调用 trace。
- `PushRequest` / `PopRequest` / `PushResponse` / `PopResponse` 调整为不再传入 ring trace 参数。

更新后的关键片段示例：
```cpp
template <typename EntryType>
StatusCode PushRingEntry(Session::RingAccess access, const EntryType& entry) {
  if (access.cursor == nullptr || access.entries == nullptr) {
    return StatusCode::EngineInternalError;
  }
  const uint32_t head = access.cursor->head.load(std::memory_order_acquire);
  const uint32_t tail = access.cursor->tail.load(std::memory_order_relaxed);
  if (tail - head >= access.cursor->capacity) {
    return StatusCode::QueueFull;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  entries[tail % access.cursor->capacity] = entry;
  access.cursor->tail.store(tail + 1u, std::memory_order_release);
  return StatusCode::Ok;
}

template <typename EntryType>
bool PopRingEntry(Session::RingAccess access, EntryType* entry) {
  if (entry == nullptr || access.cursor == nullptr || access.entries == nullptr) {
    return false;
  }
  const uint32_t tail = access.cursor->tail.load(std::memory_order_acquire);
  const uint32_t head = access.cursor->head.load(std::memory_order_relaxed);
  if (tail == head) {
    return false;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  *entry = entries[head % access.cursor->capacity];
  access.cursor->head.store(head + 1u, std::memory_order_release);
  return true;
}
```

删除测试文件：
- `rm -f tests/memrpc/rpc_client_integration_test.cpp`
- `rm -f tests/memrpc/response_queue_event_test.cpp`

`tests/memrpc/CMakeLists.txt` 删除：
```cmake
memrpc_add_test(memrpc_rpc_client_integration_test rpc_client_integration_test.cpp)
memrpc_add_test(memrpc_response_queue_event_test response_queue_event_test.cpp)
```

**Step 4: 重新编译确认通过**

Run: `cmake --build build`
Expected: 编译通过。

**Step 5: Commit**

```bash
git add src/core/session.cpp tests/memrpc/CMakeLists.txt
# 删除文件也要 add -A
git add -A src/core/session_test_hook.h tests/memrpc/rpc_client_integration_test.cpp \
  tests/memrpc/response_queue_event_test.cpp

git commit -m "feat: remove ring trace hooks and tests"
```

---

### Task 2: 移除 fd 失败注入 hook 与依赖测试

**Files:**
- Modify: `include/memrpc/client/demo_bootstrap.h`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Delete: `tests/memrpc/bootstrap_callback_test.cpp`
- Modify: `tests/memrpc/CMakeLists.txt`

**Step 1: 删除测试接口声明（制造编译失败）**

在 `include/memrpc/client/demo_bootstrap.h` 中删除：
```cpp
void SetDupFailureAfterCountForTest(int count);
```

**Step 2: 编译确认失败**

Run: `cmake --build build`
Expected: `bootstrap_callback_test.cpp` 报错：`PosixDemoBootstrapChannel` 无成员 `SetDupFailureAfterCountForTest`。

**Step 3: 删除注入逻辑与测试**

`src/bootstrap/posix_demo_bootstrap.cpp`：
- 删除 `dup_fail_after_count` 字段。
- 删除 `DuplicateFdWithTestHook`。
- `DuplicateHandles` 只使用 `dup`：
```cpp
bool DuplicateHandles(const BootstrapHandles& source, BootstrapHandles* target) {
  if (target == nullptr) {
    return false;
  }
  *target = {};

  using FdField = int BootstrapHandles::*;
  static constexpr FdField kFdFields[] = {
      &BootstrapHandles::shm_fd,
      &BootstrapHandles::high_req_event_fd,
      &BootstrapHandles::normal_req_event_fd,
      &BootstrapHandles::resp_event_fd,
      &BootstrapHandles::req_credit_event_fd,
      &BootstrapHandles::resp_credit_event_fd,
  };

  size_t dup_count = 0;
  for (FdField field : kFdFields) {
    target->*field = dup(source.*field);
    if (target->*field < 0) {
      for (size_t j = 0; j < dup_count; ++j) {
        CloseFd(&(target->*kFdFields[j]));
      }
      return false;
    }
    ++dup_count;
  }
  target->protocol_version = source.protocol_version;
  target->session_id = source.session_id;
  return true;
}
```
- 删除 `SetDupFailureAfterCountForTest` 定义。
- 调用点改为 `DuplicateHandles(impl_->handles, handles)`。

删除测试文件：
- `rm -f tests/memrpc/bootstrap_callback_test.cpp`

`tests/memrpc/CMakeLists.txt` 删除：
```cmake
memrpc_add_test(memrpc_bootstrap_callback_test bootstrap_callback_test.cpp)
```

**Step 4: 重新编译确认通过**

Run: `cmake --build build`
Expected: 编译通过。

**Step 5: Commit**

```bash
git add include/memrpc/client/demo_bootstrap.h src/bootstrap/posix_demo_bootstrap.cpp \
  tests/memrpc/CMakeLists.txt
# 删除文件也要 add -A
git add -A tests/memrpc/bootstrap_callback_test.cpp

git commit -m "feat: remove bootstrap test hooks"
```
