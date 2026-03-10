# Lock-Free SPSC 高性能共享内存 RPC 框架改造计划

日期：2026-03-10

## 一、当前状态评估

### 已完成

- Ring buffer push/pop 已是 lock-free SPSC，使用 `__atomic_*` acquire/release 语义
- SPSC 约束有测试验证（`RingTraceRecorder` 确认每个 ring 单一生产者/消费者线程）
- eventfd 边缘触发优化已实现（仅在 ring 从空→非空时写 eventfd）
- 吞吐量基线回归测试已有
- Session 恢复、引擎死亡检测、跨进程 crash+restart 测试覆盖良好

### 核心问题

**Ring 是 lock-free 的，但 ring 之外的所有东西都不是。**

## 二、性能瓶颈详细分析

### 2.1 客户端提交路径：6 次 mutex + 1 次 syscall

```
InvokeAsync (调用线程)
  → submit_mutex          ← 锁1：入提交队列
  → submit_cv.notify_one

SubmitLoop (提交线程)
  → submit_mutex          ← 锁2：出提交队列
  → session_mutex          ← 锁3：整个提交过程都持有
    → SlotPool::mutex_     ← 锁4：Reserve
    → memcpy payload
    → pending_mutex        ← 锁5：插入 pending_calls map
    → SlotPool::mutex_     ← 锁6：Transition
    → PushRequest          ← lock-free（唯一不用锁的）
    → write(eventfd)       ← syscall
```

`session_mutex` 临界区覆盖从 slot 分配到 ring push 全过程，阻塞所有需要检查 session 状态的操作。

### 2.2 响应完成路径：5 次 mutex + 2 次 syscall

```
ResponseLoop (客户端响应线程)
  → PopResponse            ← lock-free
  → pending_mutex           ← 锁1：查找 pending call
  → State::mutex            ← 锁2：设置 reply + signal cv
  → SlotPool::mutex_        ← 锁3：Release
  → pthread_mutex_t (跨进程) ← 锁4：释放共享 response slot
  → pending_mutex           ← 锁5：从 map 中删除
  → write(eventfd) × 2      ← 2次 syscall (req_credit + resp_credit)
```

### 2.3 服务端分发路径：thundering herd

```
DispatcherLoop
  → PopRequest              ← lock-free
  → WorkerPool::mutex_      ← 锁：入 worker 队列
  → cv_.notify_one

WorkerLoop (N 个线程)
  → WorkerPool::mutex_      ← N 个线程抢同一把锁
  → 完成后 cv_.notify_all   ← 唤醒所有 worker（thundering herd）
```

### 2.4 SharedSlotPool（跨进程 response slot pool）

- 使用 `pthread_mutex_t` (process-shared)，每次响应完成都要跨进程争抢
- `slot_pool.cpp` 中的 `LockSharedMutex` 没有超时，对端 crash 时持有锁会永久阻塞

### 2.5 100ms 延迟悬崖

客户端 `ResponseLoop` 和服务端 `DispatcherLoop` 都用 `poll(fd, 100ms)` 等待。在持续高负载下 ring 可能一直非空，不产生新 eventfd 信号，消费者要等到 100ms 超时才醒来。

### 2.6 RingCursor 类型不正确

`shm_layout.h` 中 `head`/`tail` 声明为 plain `uint32_t`，通过 `__atomic_*` GCC 内建操作。C++ 标准中是 UB，虽然 x86/ARM64 实际可行但不可移植。

## 三、性能差距量化

### 当前开销估算（单次 RPC 往返）

| 开销来源 | 估算 |
|---|---|
| 6 次 mutex lock/unlock（提交路径） | ~300-600 ns |
| 5 次 mutex lock/unlock（完成路径） | ~250-500 ns |
| 2-3 次 eventfd write syscall | ~2-4 μs |
| 1 次 eventfd poll 唤醒 | ~1-2 μs |
| memcpy payload（4KB max） | ~100-200 ns |
| pending_calls unordered_map 操作 | ~100-300 ns |
| pthread_mutex_t 跨进程 | ~500 ns - 2 μs |
| **总计** | **~5-10 μs** |

进程内直接调用 ~10 ns vs 当前 ~5-10 μs → **差距约 500-1000 倍**。

### 优化后理论最低开销

| 开销来源 | 估算 |
|---|---|
| SPSC ring push/pop（atomic acquire/release） | ~10-20 ns |
| eventfd write（仅在必要时） | ~1-2 μs |
| memcpy payload | ~100-200 ns |
| **理论最低** | **~1.5-2.5 μs** |

优化后差距缩小到 ~100-200 倍，主要瓶颈为 eventfd syscall（跨进程通知的固有成本）。

## 四、实施计划

### 阶段一：Lock-Free SPSC 核心改造（最高优先级）

#### 1.1 RingCursor 改为 `std::atomic<uint32_t>`

**文件**：`src/core/shm_layout.h`, `src/core/session.cpp`

- 将 `RingCursor::head`/`tail` 改为 `alignas(64) std::atomic<uint32_t>`
- 加 cache line padding 防止 false sharing（生产者写 tail，消费者写 head，应在不同 cache line）
- 将 `__atomic_*` 内建改为 `std::atomic` 的 `.load(memory_order_acquire)` / `.store(val, memory_order_release)`
- 验证 `std::atomic<uint32_t>::is_always_lock_free` 编译期断言

#### 1.2 SlotPool 改为 lock-free

**文件**：`src/core/slot_pool.h`, `src/core/slot_pool.cpp`

- 服务端本地 `SlotPool`：slot 只在两个线程间流转（submit thread 分配 → response thread 释放），改用 SPSC ring 做 slot 回收
- 状态机转换用 atomic CAS：`SlotState` 用 `std::atomic<uint8_t>`，每个 slot 独立原子变量
- `Reserve`：从 free SPSC ring pop，CAS 设置 state → `Reserved`
- `Release`：CAS 设置 state → `Free`，push 回 free SPSC ring

#### 1.3 SharedSlotPool 改为 lock-free CAS stack

**文件**：`src/core/slot_pool.h`, `src/core/slot_pool.cpp`, `src/core/shm_layout.h`

- 用 `std::atomic<uint32_t>` 做 lock-free Treiber stack（CAS push/pop）
- 替换掉 `pthread_mutex_t`，消除跨进程 mutex 和无限阻塞风险
- `SharedSlotPoolHeader` 中 `top` 用 atomic，`free_slots[]` 数组中每个 slot 存 `next` 指针（索引形式）
- ABA 问题：使用 `std::atomic<uint64_t>` 打包 `{top_index:32, version:32}`，64 位 CAS

#### 1.4 pending_calls 改为固定大小数组

**文件**：`src/client/rpc_client.cpp`

- `pending_calls` 当前是 `unordered_map<uint64_t, shared_ptr<State>>`
- 同时在飞的请求数不超过 `slot_count`（128），改为固定大小数组
- 以 `slot_index` 为索引（而非 request_id），slot 分配时就确定位置
- 数组元素用 `std::atomic<State*>` 或直接内嵌 State
- 消除 map 查找、哈希计算、动态内存分配

#### 1.5 减少 session_mutex 范围

**文件**：`src/client/rpc_client.cpp`

- session 活跃性检查改为 `std::atomic<bool> session_alive`
- `SubmitOne` 临界区拆分：
  - slot 分配：lock-free（阶段 1.2 完成后）
  - payload memcpy：无需锁（slot 已被当前线程独占）
  - pending 注册：lock-free（阶段 1.4 完成后）
  - ring push：lock-free（已是）
- 理想目标：**提交热路径上零 mutex**

### 阶段二：减少 Syscall 和唤醒开销

#### 2.1 Adaptive spinning before eventfd

**文件**：`src/client/rpc_client.cpp`, `src/server/rpc_server.cpp`

- 在 `poll(eventfd, 100ms)` 之前先 spin-wait（如 1-10 μs，可配置）
- spin 期间检查 ring 是否有新数据（load-acquire on cursor）
- 低延迟场景避免 syscall；高吞吐场景自动退化为 poll
- 实现参考：

```cpp
// 伪代码
for (int i = 0; i < kSpinCount; ++i) {
    if (RingHasData(cursor)) return true;
    _mm_pause();  // x86 或 __yield() ARM
}
// spin 超时，退化为 poll
poll(eventfd, timeout_ms);
```

#### 2.2 修复 100ms 延迟悬崖

**文件**：`src/client/rpc_client.cpp`, `src/server/rpc_server.cpp`

三个候选方案（选一）：

- **方案 A（推荐）**：每 N 个 push 强制写一次 eventfd（批量通知），N 可配置
- 方案 B：消费者 spin 一段时间后再 poll（与 2.1 合并）
- 方案 C：使用 `futex` 替代 eventfd（延迟更低，但 Linux 特定）

#### 2.3 eventfd write 合并

**文件**：`src/client/rpc_client.cpp`

- 响应完成时 2 次 eventfd write（`req_credit` + `resp_credit`）合并为条件性单次写入
- 只在对端确实在等待时才写（通过 `waiting_for_credit` atomic hint 判断）

#### 2.4 WorkerPool 改为 lock-free MPSC queue + notify_one

**文件**：`src/server/rpc_server.cpp`

- 替换 `std::queue` + `std::mutex` 为 lock-free MPSC queue（dispatcher 是唯一 producer，但多 worker 并发 pop → 实际是 SPMC）
- 或者：dispatcher 按 round-robin 分配到每个 worker 的 SPSC queue
- 用 `notify_one()` 替代 `notify_all()` 消除 thundering herd

### 阶段三：性能与稳定性测试体系

#### 3.1 进程内直接调用基线

**文件**：新建 `tests/apps/minirpc/minirpc_baseline_test.cpp`

- 直接调用 handler 函数，不走 RPC，作为性能上限参考
- 相同 payload 编解码开销，但不经过 ring/slot/eventfd
- 输出 ops/sec，与 RPC throughput test 做对比

#### 3.2 延迟分布测试

**文件**：新建 `tests/apps/minirpc/minirpc_latency_test.cpp`

- 单线程串行发送 RPC，记录每次往返时间
- 计算并输出 p50 / p99 / p999 / max 延迟
- 按 payload 大小分组：0B, 64B, 512B, 4KB
- 与进程内直接调用做对比

#### 3.3 异步管道吞吐量测试

**文件**：扩展 `tests/apps/minirpc/minirpc_throughput_test.cpp`

- 新增 async pipeline 模式：批量发出 N 个 InvokeAsync，然后批量收集 future
- 测量 async 模式 vs sync 模式的吞吐量差异
- 测试不同 batch size（1, 8, 32, 128）的影响

#### 3.4 跨进程吞吐量测试

**文件**：新建 `tests/apps/minirpc/minirpc_cross_process_test.cpp`

- 真正 fork 子进程运行 server，父进程运行 client
- 测量跨进程 RPC 吞吐量和延迟
- 与 in-process 测试结果对比，量化进程间开销

#### 3.5 长时间稳定性 soak test

**文件**：新建 `tests/apps/minirpc/minirpc_soak_test.cpp`

- 多线程持续发送 RPC，运行数分钟（可配置，默认 60s）
- 监控：slot 使用数稳定、fd 不泄漏、内存不增长
- 期间注入引擎 crash + restart（每 10s 一次）
- 验证恢复后吞吐量恢复正常

#### 3.6 背压与 slot 耗尽压力测试

**文件**：新建 `tests/apps/minirpc/minirpc_backpressure_test.cpp`

- 配置少量 slot（如 4 个）+ 慢 handler（sleep 10ms）
- 多线程并发发送
- 验证：admission timeout 正确触发、credit 机制正常工作、无 slot 泄漏
- 组合场景：slot 耗尽 + 引擎死亡

### 阶段四：VPS App 集成准备

#### 4.1 修复 VirusEngineService 的 mutex 粒度

**文件**：`src/apps/vps/child/virus_engine_service.cpp`

- 当前 `ScanFile` 持有 `mutex_` 贯穿整个扫描过程
- 多个 worker 线程形同虚设，全部串行化
- 改为按 engine 粒度加锁，或使用 read-write lock（扫描用 shared lock，Init/DeInit 用 exclusive lock）

#### 4.2 修复 VirusEngineManager 的数据竞争

**文件**：`src/apps/vps/parent/virus_engine_manager.cpp`

- `is_initialized_` 和 `reportsEnabled_` 无保护写入（行 103, 112, 173, 181）
- 改为 `std::atomic<bool>`

#### 4.3 VPS payload 大小验证

**文件**：`src/apps/vps/common/vps_codec.cpp`, `src/core/protocol.h`

- 4KB 限制对大 `ScanTask`（多个 fileInfos）可能不够
- 方案 A：增大 slot 大小（如 16KB 或 64KB）
- 方案 B：大 payload 走独立共享内存通道（out-of-band）
- 方案 C：分片传输（复杂度高，不推荐）
- 先在 VPS codec 中加 payload 大小断言，量化实际使用量后决定

#### 4.4 VPS 端到端集成测试

**文件**：新建 `tests/apps/vps/vps_integration_test.cpp`

- 用真实的 VPS handler + fork 跨进程测试
- 覆盖所有 8 个 opcode 的跨进程调用
- 验证 codec 正确性和 payload 大小在限制范围内

## 五、实施顺序与依赖关系

```
阶段一（2-3 周）                          阶段三（与阶段一并行）
  1.1 RingCursor → std::atomic              3.1 进程内直接调用基线
       ↓                                    3.2 延迟分布测试
  1.4 pending_calls → 固定数组              3.3 异步管道吞吐量
       ↓
  1.2 SlotPool → lock-free
       ↓
  1.3 SharedSlotPool → lock-free CAS
       ↓
  1.5 session_mutex 范围缩小
       ↓
阶段二（1-2 周）
  2.1 Adaptive spinning
  2.2 修复 100ms 悬崖
  2.3 eventfd write 合并
  2.4 WorkerPool lock-free
       ↓
阶段四（1 周）
  4.1 VPS mutex 粒度修复
  4.2 VPS 数据竞争修复
  4.3 payload 大小验证
  4.4 VPS 端到端测试
       ↓
  3.4 跨进程吞吐量测试
  3.5 soak test
  3.6 背压压力测试
```

## 六、关键设计决策记录

| 决策点 | 选择 | 理由 |
|---|---|---|
| Ring cursor 类型 | `std::atomic<uint32_t>` | 消除 UB，保持 lock-free，编译期可验证 |
| SlotPool 结构 | SPSC ring 回收 | 生产者和消费者固定为两个线程，SPSC 最简且最快 |
| SharedSlotPool 结构 | Lock-free Treiber stack + 64 位 CAS | 跨进程安全，消除 pthread_mutex，解决 ABA |
| pending_calls 结构 | 固定数组[slot_count] | 同时在飞请求 ≤ slot_count，O(1) 查找，零分配 |
| 唤醒策略 | Adaptive spin + eventfd fallback | 平衡延迟和 CPU 占用 |
| WorkerPool 分发 | Per-worker SPSC queue + round-robin | 消除共享锁和 thundering herd |
| VPS payload 策略 | 先量化再决定 | 避免过度设计，实际 payload 可能在 4KB 内 |

## 七、成功标准

| 指标 | 当前估算 | 目标 |
|---|---|---|
| 单次 RPC 往返延迟 | ~5-10 μs | < 3 μs（不含 eventfd 唤醒） |
| p99 延迟 | 未测量 | < 10 μs |
| 热路径 mutex 获取次数 | 11 次/RPC | 0 次/RPC |
| 热路径 syscall 次数 | 3 次/RPC | 0-1 次/RPC（spin 命中时为 0） |
| 吞吐量（Echo, 4 线程） | 基线 TBD | > 2x 基线 |
| soak test 稳定性 | 无测试 | 60s 无 leak/crash |
| 与进程内直接调用差距 | ~500-1000x | < 200x |
