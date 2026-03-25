# 代码优化指南

## 目标
1. **inline 函数不超过 10 行**
2. **圈复杂度不超过 6**
3. **函数行数不超过 50 行**

## 一、inline 函数优化

### 需要优化的 inline 函数

#### 1. `memrpc/include/memrpc/core/runtime_utils.h`

**问题函数：DrainEventFd (18行)**
```cpp
// 优化前：18行
[[nodiscard]] inline bool DrainEventFd(int fd) {
  bool drained = false;
  while (true) {
    uint64_t counter = 0;
    const ssize_t read_bytes = read(fd, &counter, sizeof(counter));
    if (read_bytes == sizeof(counter)) {
      drained = true;
      continue;
    }
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return drained;
    }
    return false;
  }
}
```

**优化方案：**
```cpp
// runtime_utils.h - 声明移至cpp文件
[[nodiscard]] bool DrainEventFd(int fd);

// runtime_utils.cpp - 实现
bool DrainEventFd(int fd) {
  bool drained = false;
  while (true) {
    uint64_t counter = 0;
    const ssize_t read_bytes = read(fd, &counter, sizeof(counter));
    if (read_bytes == sizeof(counter)) {
      drained = true;
      continue;
    }
    if (read_bytes < 0 && errno == EINTR) continue;
    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return drained;
    return false;
  }
}
```

**问题函数：SignalEventFd (16行)**
```cpp
// 优化前：16行  
[[nodiscard]] inline bool SignalEventFd(int fd) {
  if (fd < 0) return false;
  constexpr uint64_t signalValue = 1;
  while (true) {
    const ssize_t write_bytes = write(fd, &signalValue, sizeof(signalValue));
    if (write_bytes == sizeof(signalValue)) return true;
    if (write_bytes < 0 && errno == EINTR) continue;
    return false;
  }
}
```

**优化方案：**
```cpp
// runtime_utils.h
[[nodiscard]] bool SignalEventFd(int fd);

// runtime_utils.cpp
bool SignalEventFd(int fd) {
  if (fd < 0) return false;
  constexpr uint64_t signalValue = 1;
  while (true) {
    const ssize_t write_bytes = write(fd, &signalValue, sizeof(signalValue));
    if (write_bytes == sizeof(signalValue)) return true;
    if (write_bytes < 0 && errno == EINTR) continue;
    return false;
  }
}
```

**问题函数：PollEventFd (20行)**
```cpp
// 优化前：20行
[[nodiscard]] inline PollEventFdResult PollEventFd(pollfd* fd, int timeout_ms) {
  if (fd == nullptr) return PollEventFdResult::Failed;
  fd->revents = 0;
  const int poll_result = poll(fd, 1, timeout_ms);
  if (poll_result == 0) return PollEventFdResult::Timeout;
  if (poll_result < 0) {
    return errno == EINTR ? PollEventFdResult::Retry : PollEventFdResult::Failed;
  }
  if ((fd->revents & kPollFailureEvents) != 0) return PollEventFdResult::Failed;
  if ((fd->revents & POLLIN) == 0) return PollEventFdResult::Retry;
  return DrainEventFd(fd->fd) ? PollEventFdResult::Ready : PollEventFdResult::Retry;
}
```

**优化方案：** 移到cpp文件

#### 2. `memrpc/src/core/shm_layout.h`

**问题函数：ComputeLayout (15行)**
```cpp
// 优化前：15行
inline Layout ComputeLayout(const LayoutConfig& config) {
  Layout layout;
  layout.highRingOffset = AlignOffset(sizeof(SharedMemoryHeader), alignof(RequestRingEntry));
  layout.normalRingOffset = AlignOffset(
      layout.highRingOffset + sizeof(RequestRingEntry) * config.highRingSize,
      alignof(RequestRingEntry));
  layout.responseRingOffset = AlignOffset(
      layout.normalRingOffset + sizeof(RequestRingEntry) * config.normalRingSize,
      alignof(ResponseRingEntry));
  layout.totalSize = AlignOffset(
      layout.responseRingOffset + sizeof(ResponseRingEntry) * config.responseRingSize,
      alignof(std::max_align_t));
  return layout;
}
```

**优化方案：** 移到 `shm_layout.cpp`
```cpp
// shm_layout.cpp
Layout ComputeLayout(const LayoutConfig& config) {
  Layout layout;
  layout.highRingOffset = AlignOffset(sizeof(SharedMemoryHeader), alignof(RequestRingEntry));
  layout.normalRingOffset = AlignOffset(
      layout.highRingOffset + sizeof(RequestRingEntry) * config.highRingSize,
      alignof(RequestRingEntry));
  layout.responseRingOffset = AlignOffset(
      layout.normalRingOffset + sizeof(RequestRingEntry) * config.normalRingSize,
      alignof(ResponseRingEntry));
  layout.totalSize = AlignOffset(
      layout.responseRingOffset + sizeof(ResponseRingEntry) * config.responseRingSize,
      alignof(std::max_align_t));
  return layout;
}
```

#### 3. `virus_executor_service/include/transport/ves_control_stub.h`

**问题函数：DecodeAnyCallRequest (17行), EncodeAnyCallReply (15行)**

这些应该移到 `ves_control_stub.cpp` 文件中。

---

## 二、长函数拆分（超过50行）

### 1. `rpc_client.cpp` - SubmitOne (约100行)

**拆分策略：**
```cpp
// 原函数：SubmitOne (约100行)
// 拆分为以下辅助函数：

// 1. 准备提交信息 (约15行)
PendingInfo PrepareSubmitInfo(const PendingSubmit& submit);

// 2. 处理会话状态恢复 (约25行)
StatusCode HandleSessionRecovery(const PendingSubmit& submit, 
                                  std::chrono::steady_clock::time_point deadline,
                                  std::chrono::steady_clock::time_point recoveryDeadline);

// 3. 处理推送结果 (约30行)
bool HandlePushResult(StatusCode pushStatus, const PendingSubmit& submit,
                      const PendingInfo& info);

// 4. 简化后的 SubmitOne (约30行)
void SubmitOne(const PendingSubmit& submit) {
    const PendingInfo info = MakePendingInfo(submit);
    
    while (running_.load(std::memory_order_acquire)) {
        if (auto status = HandleSessionState(submit); status != StatusCode::Ok) {
            FailAndResolve(info, status, FailureStage::Admission, submit.future);
            return;
        }
        
        if (auto pushStatus = TryPushRequest(submit); pushStatus == StatusCode::Ok) {
            return;
        } else if (!HandlePushRetry(pushStatus, submit, info)) {
            return;
        }
    }
    
    FailAndResolve(info, StatusCode::PeerDisconnected, FailureStage::Admission, submit.future);
}
```

### 2. `rpc_client.cpp` - EnsureLiveSession (约50行)

**拆分策略：**
```cpp
// 拆分出以下辅助函数：

// 1. 检查前置条件
bool CheckPreconditions(StatusCode& outStatus);

// 2. 处理状态转换
void TransitionToRecovering(ClientLifecycleState currentState);

// 3. 处理打开失败
void HandleOpenFailure(StatusCode status, RecoveryTrigger trigger);

// 优化后的 EnsureLiveSession (约30行)
StatusCode EnsureLiveSession() {
    StatusCode status;
    if (!CheckPreconditions(status)) return status;
    
    if (auto snapshot = LoadSessionSnapshot(); snapshot->alive) {
        return StatusCode::Ok;
    }
    
    TransitionToRecovering(LifecycleState());
    
    if (StatusCode status = OpenSession(); status != StatusCode::Ok) {
        HandleOpenFailure(status, PendingSessionOpenTrigger());
        return status;
    }
    return StatusCode::Ok;
}
```

### 3. `rpc_server.cpp` - ProcessEntry / WriteResponse / PublishEvent

类似策略：将大函数拆分为多个小的、单一职责的辅助函数。

---

## 三、圈复杂度优化（超过6）

### 1. 简化复杂条件判断

**优化前：**
```cpp
if (pushStatus == StatusCode::Ok) {
    return;
}
if (pushStatus == StatusCode::PayloadTooLarge) {
    // ...
    return;
}
if (pushStatus == StatusCode::PeerDisconnected) {
    // ...
    return;
}
if (pushStatus != StatusCode::QueueFull) {
    // ...
    return;
}
// ...
```

**优化后（使用查找表）：**
```cpp
enum class PushAction { Return, FailAndReturn, Retry, WaitForCredit };

static PushAction GetPushAction(StatusCode status) {
    switch (status) {
        case StatusCode::Ok: return PushAction::Return;
        case StatusCode::PayloadTooLarge: return PushAction::FailAndReturn;
        case StatusCode::PeerDisconnected: return PushAction::Retry;
        case StatusCode::QueueFull: return PushAction::WaitForCredit;
        default: return PushAction::FailAndReturn;
    }
}
```

### 2. 提取早期返回

**优化前：**
```cpp
void SomeFunction() {
    if (condition1) {
        if (condition2) {
            if (condition3) {
                // 实际逻辑
            } else {
                return;
            }
        } else {
            return;
        }
    } else {
        return;
    }
}
```

**优化后：**
```cpp
void SomeFunction() {
    if (!condition1) return;
    if (!condition2) return;
    if (!condition3) return;
    // 实际逻辑
}
```

---

## 四、优化执行步骤

### 步骤1：移动非性能关键的 inline 函数
1. 创建/更新对应的 `.cpp` 文件
2. 将超过10行的 inline 函数移至实现文件
3. 保留性能关键的小型 inline 函数（如 AtomicLoad/Store）

### 步骤2：拆分长函数
1. 识别超过50行的函数
2. 提取辅助函数
3. 确保每个函数单一职责

### 步骤3：降低圈复杂度
1. 使用查找表替代复杂 switch/if-else
2. 提取早期返回
3. 使用策略模式替代复杂条件分支

### 步骤4：验证
```bash
# 启用新的 clang-tidy 检查
tools/build_and_test.sh --tidy
```

---

## 五、优先处理的文件清单

按优先级排序：

1. **memrpc/include/memrpc/core/runtime_utils.h**
   - DrainEventFd, SignalEventFd, PollEventFd → runtime_utils.cpp

2. **memrpc/src/core/shm_layout.h**
   - ComputeLayout → shm_layout.cpp

3. **virus_executor_service/include/transport/ves_control_stub.h**
   - DecodeAnyCallRequest, EncodeAnyCallReply → ves_control_stub.cpp

4. **memrpc/src/client/rpc_client.cpp**
   - SubmitOne, EnsureLiveSession 拆分为小函数

5. **memrpc/src/server/rpc_server.cpp**
   - ProcessEntry, WriteResponse, PublishEvent 拆分为小函数

6. **virus_executor_service/src/transport/ves_control_proxy.cpp**
   - 检查并拆分长函数
