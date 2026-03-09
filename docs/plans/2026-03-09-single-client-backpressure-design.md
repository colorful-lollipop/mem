# 单 Client/Server 背压与执行状态设计

## 目标

在明确约束为“单 client + 单 server + 单 session”的前提下，调整 `memrpc` 的过载处理、超时语义和共享内存状态记录方式，使框架从“可工作的原型”收敛到更适合生产化演进的基础形态。

本轮设计重点解决三件事：

- 让真正的背压停在 client 侧，而不是被 server 进程内无界队列吞掉
- 把“等待入队”“服务端排队”“handler 执行”三段时间预算拆开
- 在共享内存里记录请求执行阶段，便于 crash 或卡死后定位停在哪个任务

## 前提与边界

本设计明确接受以下约束：

- 当前只支持单 client
- 当前只支持单 server
- 同一时刻只存在一个有效 session
- 不在本轮扩展多 client 竞争语义
- 不在本轮引入真正的 zero-copy response 协议

这意味着框架需要把“单 client / 单 server”写成显式设计约束，而不是默认假设。

## 现状问题

当前主链路可以工作，但有几个关键问题：

- server dispatcher 会把共享内存 request ring 持续 drain 到进程内 `std::queue`
- 当 worker 全忙时，背压点从共享内存转移到了 server 进程内内存
- `queue_timeout_ms` 只覆盖“进入 worker 前”的服务端排队时间，client 侧 admission wait 没有独立预算
- `exec_timeout_ms` 只是软超时，handler 跑完后才折叠为 `ExecTimeout`
- 共享内存里没有稳定的 in-flight 状态记录，crash 后只能知道 session 坏了，不能快速判断卡在排队还是执行中

## 设计结论

### 1. 背压应优先停在 client 侧

保留 server 线程池，但 server 不应继续把共享队列无界搬运到本地队列。

推荐行为：

- worker 有空闲时，server 从共享 request ring 取任务
- worker 全忙但本地等待队列仍有容量时，server 可以继续少量取任务
- 本地等待队列也满时，server 停止继续 drain 共享 request ring
- client 看到 slot 不足或 request ring 满时，按 admission budget 等待重试

这样共享 request ring 才是整个系统真正的背压点。

### 2. 超时语义拆成三段

当前 `queue_timeout_ms` 和 `exec_timeout_ms` 不足以表达完整路径。建议拆成：

- `admission_timeout_ms`
  client 侧等待“拿到 slot + 成功写入 request ring”的最长时间
- `queue_timeout_ms`
  request 已经进入共享 request ring 后，到 server worker 真正开始执行前允许等待的最长时间
- `exec_timeout_ms`
  handler 真正开始执行后允许占用 worker 的最长时间

建议约定：

- `0` 表示该阶段无限等待
- 默认值应当是有限时间，不建议默认全部为 `0`

这样调用侧和排障视角都会更清楚：

- 超过 `admission_timeout_ms`，说明系统入口已经拥塞
- 超过 `queue_timeout_ms`，说明 server 侧调度或 worker 容量不足
- 超过 `exec_timeout_ms`，说明 handler 本身慢或卡住

### 3. `exec_timeout_ms` 先定义成软超时

本轮不建议直接做强制中断 handler。

原因：

- C++ 普通业务 handler 很难安全强杀
- 线程取消、异步中断或信号中断都会显著抬高复杂度和不确定性
- 当前更现实的路径是先把超时语义、状态记录和诊断补完整

因此本轮保留软超时：

- worker 开始执行时记录 `start_exec_mono_ms`
- handler 返回后计算耗时
- 如果超出 `exec_timeout_ms`，reply.status 置为 `ExecTimeout`

同时在文档和代码注释里明确：这不是强制中断，只是结果折叠。

## 共享内存状态设计

### 1. 以 slot 为主状态载体

不建议主设计采用单独的“最多 10 个任务 id 数组”来记录在执行任务。

更合适的方式是：每个请求本来就和 `slot_index` 强绑定，因此把执行状态直接放在 slot 元数据里。

建议为每个 slot 增加一段元数据，例如：

- `request_id`
- `state`
- `worker_id`
- `enqueue_mono_ms`
- `start_exec_mono_ms`
- `last_heartbeat_mono_ms`

建议状态枚举：

- `Free`
- `Admitted`
- `Queued`
- `Executing`
- `Responding`

其中：

- `Admitted` 表示 client 已拿到 slot，正在准备写请求
- `Queued` 表示请求已进入共享 request ring
- `Executing` 表示 worker 已经真正开始处理
- `Responding` 表示 reply 正在写回

### 2. 可选增加 worker 诊断表

如果已知 worker 数量不超过 10，可以额外在共享内存里放一个小的 `worker_state[10]` 调试区，但它只作为诊断辅助，不作为主状态来源。

每个 `worker_state` 可以记录：

- `busy`
- `worker_id`
- `slot_index`
- `last_heartbeat_mono_ms`

它的用途是：

- 快速看到当前哪些 worker 忙
- crash dump 或现场排障时，辅助判断哪条线程最后卡在哪个 slot

但主状态仍应以 slot 元数据为准，避免双份真相。

## Client 行为调整

### 1. Admission Wait

`RpcClient::InvokeAsync()` 不再在以下情况直接返回：

- `SlotPool::Reserve()` 失败
- `session.PushRequest()` 返回 `QueueFull`

而是改成：

- 记录 admission deadline
- 在 deadline 内循环重试
- 每次失败后短暂等待条件满足再重试
- 超过 deadline 返回 `QueueFull` 或新的 `AdmissionTimeout`

是否新增 `AdmissionTimeout` 状态码，有两种可选方案：

- 方案 A：继续返回 `QueueFull`
- 方案 B：新增 `AdmissionTimeout`

推荐方案 B，因为它能和“瞬时队列满”区分开。

### 2. Client 无限等待的语义

支持：

- `admission_timeout_ms = 0` 表示 client 无限等待入队机会

但默认值仍建议有上限。无限等待应由业务显式选择。

### 3. 单 Client 约束显式化

既然当前只支持单 client，就需要在 bootstrap 或 session 建立阶段明确拒绝第二个 client attach。

建议：

- 在共享内存 header 里增加 `client_attached` 或 `active_client_session_id`
- 第一个 client attach 成功后占有该 session
- 第二个 client attach 直接失败并返回明确状态

这样框架边界是显式的，不会留下“貌似能连，实际上会踩 slot/request_id”的隐患。

## Server 行为调整

### 1. 保留线程池，但让线程池成为真实容量边界

server 继续保留高优/普通 worker 池。

但 dispatcher 不应无限制 `DrainQueue()` 到本地 `std::queue`。建议改成两种实现之一：

- 方案 A：worker 线程直接从共享 request ring 取任务
- 方案 B：保留本地队列，但本地队列改成有界队列

推荐方案 B 作为最小改动路径。原因：

- 现有 worker pool 结构可以保留
- 改动面小于“worker 直接碰共享内存 ring”
- 更容易逐步落地和测试

建议本地等待队列容量：

- 默认等于对应 worker 数量
- 或者等于 worker 数量的 1 到 2 倍

一旦本地队列满，dispatcher 本轮停止继续 drain，对应背压自然回落到共享 request ring。

### 2. Queue Timeout 的判定点

`queue_timeout_ms` 的计时起点仍保留为：

- client 成功 push `RequestRingEntry` 时写入的 `enqueue_mono_ms`

超时判定时机：

- worker 线程真正准备开始执行 handler 前

这能保证语义稳定：

- 已经进入共享队列但长时间没有被执行，才算 `QueueTimeout`

### 3. 执行态与心跳

worker 取到任务后：

- 把对应 slot 状态更新为 `Executing`
- 写入 `worker_id`
- 记录 `start_exec_mono_ms`

如果 handler 可能长时间执行，可选增加轻量心跳更新：

- 每个 worker 周期性更新 `last_heartbeat_mono_ms`

这样 crash、hang 或 watchdog 排查时，能更快区分：

- 是队列堵了
- 是 worker 全忙
- 还是某个 handler 卡住了

## Crash 与恢复语义

当子进程 crash 或 session 判定损坏时，目标不只是“通知 client 失败”，还要尽可能保留现场信息。

建议：

- crash 前无法保证写回 reply，但共享内存 slot 元数据应尽量保留最后状态
- client 在收到死亡通知或 session broken 后，统一失败 pending future
- 新 session 建立时不尝试复用旧 slot 元数据，只把它当上一次现场

这意味着 slot 元数据主要服务于：

- crash 后诊断
- hang 分析
- 运行态观测

而不是作为跨 session 恢复来源。

## 兼容与迁移策略

本轮建议分阶段迁移，避免一次性改太多：

### Phase 1

- 增加 `admission_timeout_ms`
- 增加 slot 执行状态元数据
- 保持现有 worker pool，先不改 response 协议

### Phase 2

- 把 server 本地等待队列改成有界
- 停止无界 drain 共享 request ring
- 补齐 admission / queue / exec 三段超时测试

### Phase 3

- 评估是否需要增加 `worker_state[10]`
- 评估是否需要将 response 侧改为 slot/buffer descriptor 以减少拷贝

## 测试重点

必须覆盖：

- 单 client attach 成功，第二个 client attach 明确失败
- worker 全忙且本地等待队列满时，server 不再继续 drain 共享 request ring
- client 在 admission wait 后成功入队
- client admission wait 超时后返回预期错误
- request 进入共享队列后，超出 `queue_timeout_ms` 返回 `QueueTimeout`
- handler 执行超出 `exec_timeout_ms` 后返回 `ExecTimeout`
- crash 后共享内存 slot 元数据能反映最后已知状态

## 结论

在“单 client + 单 server”前提下，生产化的关键不是先做多 client，也不是先做 zero-copy，而是先把三件基础设施补完整：

- 背压边界回收到 client 侧
- admission / queue / exec 三段超时拆清楚
- slot 级执行状态进入共享内存

这三点补齐后，当前共享内存 RPC 框架才更接近可以承载真实生产流量的基础形态。
