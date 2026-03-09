# 单 Client/Server 共享内存 RPC 生产化路线图设计

## 目标

在明确约束为“单 client + 单 server + 单 session”的前提下，把当前 `memrpc` 从可工作的共享内存 RPC 原型收敛到更接近生产级的实现。

本轮不追求通用多 client 框架，不追求 lock-free，不追求一次性重写协议。重点是把背压、超时、在途任务状态和协议对称性收紧到可维护、可诊断的水平。

## 当前结论

当前主线有可用骨架，但距离最佳实践仍有明显差距：

- 背压点不正确。server 会把共享请求队列持续 drain 到进程内无界 `std::queue`，过载时压力落在 server 内存，而不是停在 client/shared queue。
- 超时语义不完整。当前只有 `queue_timeout_ms` 和 `exec_timeout_ms`，缺少 client admission 等待时间。
- 请求/响应协议不对称。请求是 `request ring + request slot`，响应则内嵌在 `response ring`。
- 共享内存内缺少在途任务状态，crash 后很难知道任务停在 admission、排队还是执行阶段。
- 单 client 前提没有被框架显式写死，容易被未来误用。
- 同步等待路径仍有 busy wait，工程质感不够。

## 约束与边界

本路线图基于以下前提：

- 只支持一个活跃 client attach 到一个 server session
- server 允许多个 worker 并发执行 handler
- 共享内存仍然使用固定大小 slot，不引入动态 allocator
- 优先保证正确性、可诊断性和过载行为，再考虑减少拷贝
- 当前 `eventfd + robust mutex + shared memory ring` 模型保留，不做 lock-free 重写

## 目标形态

### 1. 背压停在 client 侧

期望模型是：

- server 仅在自己还能承接任务时，才从共享请求队列取活
- 当 worker 全忙且本地等待队列达到上限时，server 停止继续 drain 共享队列
- client 在发现 `slot` 不可用或 request ring 满时，进入 admission 等待
- admission 等待可无限，也可由超时限制

这样共享请求队列才是真正的系统背压点。

### 2. 三段超时拆分

建议把时间预算分成三个阶段：

- `admission_timeout_ms`
  client 等待 `request slot + request ring` 可用的时间
- `queue_timeout_ms`
  请求已经进入共享请求队列后，等待被 worker 真正开始处理的时间
- `exec_timeout_ms`
  handler 实际执行时间

语义要求：

- `admission_timeout_ms = 0` 表示 admission 无限等待
- `queue_timeout_ms = 0` 表示 server 端排队不限时
- `exec_timeout_ms = 0` 表示执行不限时

其中 `exec_timeout_ms` 需要明确为“软超时”还是“硬超时”。现阶段更现实的目标是先保留软超时，但必须在接口和文档中写清楚。

### 3. 在途任务状态写入共享内存

建议在 slot 元数据中记录任务状态，而不是额外维护一份独立任务数组。

每个 request slot 建议至少包含：

- `request_id`
- `state`
- `worker_id`
- `enqueue_mono_ms`
- `start_exec_mono_ms`
- `last_heartbeat_mono_ms`

`state` 建议覆盖：

- `Free`
- `Reserved`
- `Admitted`
- `Queued`
- `Executing`
- `Responding`
- `Completed`

这样在 crash 或卡死后，可以直接从共享内存判断任务停在哪一阶段。

如果后续仍希望快速看到 worker 当前状态，可以额外加一个很小的 `worker_state[]` 调试区，但它应当是辅助诊断，不应取代 slot 状态成为主事实来源。

### 4. 协议逐步走向对称

长期更合理的形态是：

- 请求：`request ring + request slot`
- 响应：`response ring + response slot`

ring 只承担排队、通知和索引职责，正文统一放在 slot 中。

这能带来：

- 请求/响应模型一致
- 响应体不再被内嵌 ring payload 大小限制死
- 回包状态与诊断信息更容易挂载
- 后续减少拷贝更自然

但这是中期改造，不适合和背压/超时改造完全捆在一个提交里。

## 优先级

### P0：必须先收紧的生产性缺口

- 明确并强制单 client ownership，禁止第二个活跃 client attach 同一 session
- 把 server 本地等待队列改成有界，或直接取消本地等待队列
- 在 client 侧引入 admission 等待与 `admission_timeout_ms`
- 把 `queue_timeout_ms` 和 `exec_timeout_ms` 的职责边界写死
- 在 request slot 中增加在途状态字段
- 去掉 `InvokeSync()` 的 busy wait，改成基于条件变量的 deadline wait

### P1：应在 P0 后推进的结构优化

- 把请求读取改成零额外拷贝的只读 view 或 decode-on-slot
- 为响应引入 response slot，去掉 response ring 内嵌 payload
- 增加 worker 调试状态区
- 补强压力、过载、慢 handler、异常恢复测试
- 增强队列深度、超时分类、slot 状态和重连过程的可观测性

## 不建议做的事

- 不建议现在就引入复杂共享内存 allocator
- 不建议为了“最佳实践”直接改成 lock-free MPMC
- 不建议把多 client 支持混进这一轮生产化改造
- 不建议继续扩大 response ring 内嵌 payload 大小来掩盖协议问题

## 分阶段建议

### 阶段 1：先收紧行为，不改大协议

先完成：

- 单 client ownership
- client admission 等待
- server 有界背压
- 三段超时语义
- request slot 在途状态
- sync wait 去 busy wait

这一阶段的目标是先把行为和排障能力拉到可接受水平。

### 阶段 2：再做协议对称化

第二阶段引入 response slot，使响应也走 `ring + slot` 模型。

这一阶段重点是：

- 统一协议模型
- 进一步减少拷贝
- 让响应路径也能被共享内存状态完整诊断

## 预期结果

完成 P0 后，框架应具备以下特征：

- 过载时，背压优先停在 client/shared queue，而不是 server 进程内存
- 超时分类清晰，日志和状态可定位
- crash 后可从共享内存判断任务停在什么阶段
- 单 client 假设被框架显式保护
- 同步等待行为更稳，不依赖忙等

完成 P1 后，框架才更接近“单 client/server 场景下的共享内存 RPC 最佳实践实现”。
