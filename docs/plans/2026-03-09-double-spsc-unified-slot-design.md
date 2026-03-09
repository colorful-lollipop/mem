# 双 SPSC + 统一 Slot 共享内存 RPC 设计

## 目标

在“单 client + 单 server + 单 session”的明确前提下，把当前 `memrpc` 收敛为更适合生产落地的共享内存 RPC 结构：

- 3 条共享 ring 都收敛为 lock-free `SPSC`
- 请求与响应统一采用 `ring + slot` 模型
- 共享内存成为真实背压点
- 在途任务状态可从共享内存直接诊断

本设计不追求通用多 client，不追求 lock-free MPMC，也不引入共享内存动态分配器。

## 背景

当前实现的主要问题不是“功能不能跑”，而是结构仍偏原型：

- 请求侧允许多个业务线程直接写共享请求队列
- 响应侧多个 worker 直接写共享响应队列
- ring 仍依赖 mutex push/pop，而不是标准 `SPSC`
- 请求与响应协议不对称
- 过载时 server 会把共享队列 drain 到进程内无界队列
- 调试时难以判断任务停在哪一阶段

这些问题叠加后，使得 lock-free 改造难度高、收益不集中。

## 方案比较

### 方案 A：双 SPSC，ring 内嵌完整 payload

结构：

- `client tx thread -> request ring -> server dispatcher`
- `server response thread -> response ring -> client rx thread`
- ring entry 内直接包含请求或响应正文

优点：

- 协议简单
- slot 生命周期不存在
- 初期实现成本最低

缺点：

- ring entry 会随最大 payload 变大
- 小请求也占大 entry
- 共享内存里不容易记录完整在途状态
- 后续扩展大响应或调试信息时会很别扭

### 方案 B：双 SPSC，统一使用 request/response slot

结构：

- `high request ring`
- `normal request ring`
- `response ring`
- `request slot pool`
- `response slot pool`

ring 只放索引和路由元数据，正文和状态放在 slot 中；3 条共享 ring 都实现为 lock-free `SPSC`。

优点：

- 请求/响应协议完全对称
- ring 保持小而稳定
- slot 可承载状态、时间戳和调试信息
- 更容易演进到更低拷贝路径

缺点：

- 协议和状态机会更复杂
- 需要维护两套 slot 生命周期

### 方案 C：双 SPSC，小消息内嵌 ring，大消息走 slot

结构：

- 小于阈值的 payload 直接放 ring
- 大于阈值的 payload 走 slot

优点：

- 小消息路径快
- 大消息不膨胀 ring

缺点：

- 协议分叉，维护成本高
- 状态追踪要兼容两条路径
- 容易把“优化”变成长期负担

## 推荐方案

推荐 **方案 B：双 SPSC + 统一使用 request/response slot**。

原因：

- 你们已经明确希望长期把框架收敛成“单 client/server 的生产级共享内存 RPC”
- 当前最需要的是结构统一、背压正确、状态可诊断
- 在这三个目标下，统一模型比混合模型更稳

方案 C 只有在已经把框架做稳、并且 benchmark 明确显示“小消息内嵌 ring”价值足够大时，才值得后续单独评估。

## 核心结构

### 1. 线程与共享内存边界

推荐线程关系：

- client 业务线程
  只构造请求，提交给 client 本地发送队列
- client 发送线程
  唯一写共享请求队列
- client 接收线程
  唯一读共享响应队列
- server dispatcher 线程
  唯一读共享请求队列
- server worker 线程池
  并发执行 handler
- server response writer 线程
  唯一写共享响应队列

共享内存边界因此变成：

- 请求方向：`client tx thread -> request rings -> server dispatcher`
- 响应方向：`server response writer -> response ring -> client rx thread`

也就是说，跨进程的 ring 操作都收敛为 `SPSC`，并且只在这 3 条共享 ring 上引入 lock-free。

### 2. Ring 布局

保留三条 ring：

- `high request ring`
- `normal request ring`
- `response ring`

其中：

- `high request ring` 和 `normal request ring` 都由 client 发送线程写、server dispatcher 读
- `response ring` 由 server response writer 写、client 接收线程读

3 条共享 ring 都采用 lock-free `SPSC` cursor 模型：

- `tail` 只由生产者写
- `head` 只由消费者写
- 生产者通过读取 `head` 判断是否已满
- 消费者通过读取 `tail` 判断是否为空

ring entry 只放：

- `request_id`
- `slot_index`
- `opcode`
- `priority`
- `flags`
- `enqueue_mono_ms`
- `message_kind`
- 必要的结果码和小元数据

ring 不直接内嵌大块 payload。

### 3. 锁边界

锁的边界建议明确如下：

- 共享 ring：无 mutex，改为 lock-free `SPSC`
- `request slot pool`：保留 pool 级细粒度锁
- `response slot pool`：保留 pool 级细粒度锁
- client 本地提交队列：保留锁
- server 本地完成队列：保留锁
- worker pool：保留锁和条件变量

`slot pool` 的锁只保护：

- `Reserve()`
- `Release()`
- free list / bitmap
- 高优保留额度计数

`slot pool` 的锁不应保护：

- slot payload 读写
- handler 执行
- slot 状态字段的整个生命周期

也就是说，slot pool 保持“小范围加锁”，不要把它扩大成大锁。

### 4. Slot Pool 布局

只保留两套 slot pool：

- `request slot pool`
- `response slot pool`

不建议拆成三套：

- 高优 request slot pool
- 普通 request slot pool
- response slot pool

原因是高优和普通请求的正文生命周期一致，只需在同一个 `request slot pool` 上做高优保护即可。

### 5. 高优保护

高优与普通请求共用 `request slot pool`，但要增加高优保留额度：

- 设 `requestSlotCount`
- 设 `highReservedRequestSlots`

普通请求最多使用：

- `requestSlotCount - highReservedRequestSlots`

高优请求可以使用全部，或者至少可使用保留区。

这样可避免普通流量把 request slot 完全耗尽，导致高优请求无法进入系统。

## 外部故障感知边界

本设计明确假设：

- 进程 crash / death 感知
- session 失效通知
- 句柄重建与重连协调

由鸿蒙框架层负责。

因此在本层：

- ring 不需要继续承担 robust mutex owner-death 语义
- 共享 ring 不再为了 crash 恢复保留 mutex
- 本层重点只放在正常流控、代际隔离和调试状态

仍建议保留 `session_id` 或等价代际标识，用于防止旧 session 的残留数据误入新 session。

## Slot 结构建议

### Request Slot

`request slot` 的总大小固定，内部有效 payload 长度可变。

建议字段：

- `request_id`
- `state`
- `opcode`
- `priority`
- `flags`
- `payload_size`
- `enqueue_mono_ms`
- `start_exec_mono_ms`
- `last_heartbeat_mono_ms`
- `worker_id`
- `payload[]`

`state` 建议取值：

- `Free`
- `Reserved`
- `EnqueuedHigh`
- `EnqueuedNormal`
- `Dequeued`
- `Executing`
- `WaitingResponseSlot`
- `Completed`

### Response Slot

`response slot` 也采用固定总大小、变长有效内容。

建议字段：

- `request_id`
- `state`
- `status_code`
- `engine_code`
- `detail_code`
- `result_size`
- `reply_mono_ms`
- `writer_mono_ms`
- `payload[]`

`state` 建议取值：

- `Free`
- `Reserved`
- `Writing`
- `Ready`
- `Consumed`

## 生命周期

### 请求生命周期

1. client 业务线程构造 `RpcCall`
2. 请求进入 client 本地发送队列
3. client 发送线程申请 `request slot`
4. client 发送线程把请求写入 `request slot`
5. client 发送线程把 `slot_index` 写入高优或普通 `request ring`
6. server dispatcher 从 `request ring` 取出 entry
7. server dispatcher 把请求交给 worker
8. worker 标记 `request slot` 为 `Executing`
9. worker 执行业务 handler
10. worker 生成结果，申请 `response slot`
11. worker 把结果交给 server response writer
12. response writer 写 `response ring`
13. client 接收线程消费 `response ring`
14. client 接收线程读取 `response slot`
15. client 接收线程完成 future，释放 `response slot`
16. 当响应已经安全落地到 client 本地对象后，释放 `request slot`

### 响应生命周期

1. worker 生成 `RpcServerReply`
2. server response writer 申请 `response slot`
3. 把结果写入 `response slot`
4. 在 `response ring` 中写入 `response_slot_index`
5. client 接收线程读取 `response ring`
6. client 接收线程根据 `response_slot_index` 读取正文
7. client 接收线程唤醒 pending future
8. client 接收线程释放 `response slot`

## 超时模型

必须拆成三段：

- `admission_timeout_ms`
  client 等待 `request slot + request ring` 可用的时间
- `queue_timeout_ms`
  请求已经进入共享请求队列后，到 worker 真正开始执行前的时间
- `exec_timeout_ms`
  handler 实际执行的时间

语义：

- `admission_timeout_ms = 0` 表示 admission 无限等待
- `queue_timeout_ms = 0` 表示排队不限时
- `exec_timeout_ms = 0` 表示执行不限时

本轮建议保留 `exec_timeout_ms` 为软超时：

- 超时后不强杀 handler
- 但需要把结果标成 `ExecTimeout`
- 必须在接口与注释里明确这一点

## 背压模型

推荐背压顺序：

1. worker 池忙
2. server 本地等待队列达到有界上限
3. server 停止继续 drain 共享请求队列
4. 共享请求队列逐渐满
5. client admission 等待被触发
6. 超过 `admission_timeout_ms` 后再失败

关键原则：

- 共享请求队列必须重新成为真实背压点
- server 不能再用无界进程内队列掩盖过载

## Slot 数量建议

### Request Slot 数量

如果 request slot 从 admission 持有到客户端收完整个响应后再释放，则建议：

`requestSlotCount >= highRingSize + normalRingSize + maxExecutingRequests + localServerBacklog`

其中：

- `maxExecutingRequests` 一般等于 worker 线程数
- `localServerBacklog` 是 server 本地有界等待队列容量

实际建议取整到便于配置的值，例如 `64 / 96 / 128`。

### Response Slot 数量

建议起步：

`responseSlotCount >= responseRingSize + maxPendingWriteCompletions`

如果 response writer 不额外积压很多结果，实践上可先取：

`responseSlotCount = responseRingSize`

### 高优保留额度

建议：

`highReservedRequestSlots >= highRingSize`

这样可以保证高优 ring 中理论可见的所有请求，都有机会配套占到 request slot。

## 共享内存状态诊断

该模型的价值之一是出现卡顿、异常延迟或调试排查时，可直接查看共享内存中的 slot 状态。

建议在诊断时重点看：

- 哪些 `request slot` 还停留在 `Enqueued*`
- 哪些 `request slot` 停留在 `Executing`
- 哪些 `response slot` 停留在 `Ready`
- `worker_id`
- `start_exec_mono_ms`
- `last_heartbeat_mono_ms`

必要时可额外增加一个很小的 `worker_state[]` 区域，记录：

- worker 是否 busy
- 当前处理的 `request_slot_index`
- 最近心跳

但这属于辅助诊断，不应替代 slot 状态本身。

## Lock-Free Ring 约束

由于本设计把共享 ring 明确收敛成 `SPSC`，因此 lock-free ring 仅需满足：

- 单生产者写 entry 后，再发布 `tail`
- 单消费者观察到 `tail` 后，再读取 entry
- 单消费者消费完成后，再发布 `head`

推荐内存序原则：

- 生产者发布 `tail` 使用 `store-release`
- 消费者读取 `tail` 使用 `load-acquire`
- 消费者发布 `head` 使用 `store-release`
- 生产者读取 `head` 使用 `load-acquire`

这一层不需要引入 CAS 风暴，也不需要实现 MPMC 算法。

## 风险与代价

引入该设计的代价主要有：

- client 多一个发送线程
- server 多一个 response writer 线程
- 3 条共享 ring 要从 mutex 版改成 lock-free `SPSC`
- 协议从不对称改成完全对称
- 需要两套 slot pool 与两套状态机
- 测试矩阵会变大

但和直接把现有结构硬改成 lock-free MPMC 相比，这个代价明显更可控，而且收益更集中。

## 迁移建议

### 阶段 1：先把并发边界收敛成双 SPSC，并切换 3 条共享 ring

先做：

- client 发送线程
- server response writer 线程
- `high request ring`
- `normal request ring`
- `response ring`
  都切换为 lock-free `SPSC`

这一阶段可以暂时保留现有 request slot / inline response 的结构，只为切换线程边界和 ring 实现服务。

### 阶段 2：引入 response slot，对称化协议

做：

- `response slot pool`
- `response ring` 改为只传索引与元数据
- client 接收线程改为从 `response slot` 读正文

### 阶段 3：补齐状态、背压和超时

做：

- request/response slot 状态字段
- admission timeout
- server 有界背压
- 高优 request slot 保留额度

## 预期结果

完成后应达到：

- 3 条跨进程共享 ring 都满足 lock-free `SPSC`
- 请求与响应协议完全对称
- 背压停留在 client/shared queue 边界
- slot 可以承载调试和状态定位所需信息
- slot pool 与本地队列仍保持低风险、细粒度加锁
