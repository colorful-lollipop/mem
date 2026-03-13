# Credit Eventfd 设计

## 目标

在保持当前 `单 client + 单 server + ring + slot` 总体结构不变的前提下，去掉 request/response 两侧“资源不足时 `sleep_for(1ms)` 重试”的轮询等待，改成基于 `eventfd` 的跨进程 credit 唤醒。

本设计不改变现有共享内存 ring 布局，不引入新的线程角色，也不引入 futex 或共享内存条件变量。

## 背景

当前实现里有两类等待仍然依赖轮询：

- client submitter 在 request admission 失败时，用 `sleep_for(1ms)` 重试
- server response writer 在 response slot 或 response ring 资源不足时，用 `sleep_for(1ms)` 重试

这种方式能工作，但存在几个问题：

- 压力大时会带来无意义 wakeup
- 会引入 1ms 级抖动
- 工业化场景下不够干净，资源恢复不是事件驱动

要彻底去掉轮询，核心不是“优化 sleep”，而是让“资源重新可用”本身变成一个可等待事件。

## 方案比较

### 方案 A：保留 3 个现有 eventfd，只做边沿合并

做法：

- 继续只保留 `highReqEventFd`、`normalReqEventFd`、`respEventFd`
- 通过“从空变非空时才写 fd”减少 write 次数

优点：

- 协议改动最小
- syscall 数量会下降

缺点：

- 只能减少无效唤醒
- 资源不足时仍然只能 sleep-retry
- 不能真正去掉轮询

### 方案 B：新增 2 个 credit eventfd

做法：

- 保留现有 3 个“有新消息可读”的 fd
- 新增 2 个“资源重新可写”的 fd

优点：

- 请求侧和响应侧都能去掉 1ms 轮询
- fd 语义清晰
- 和现有 `eventfd + poll` 模型一致

缺点：

- 需要改 `BootstrapHandles`、bootstrap、session、client/server 两侧逻辑

### 方案 C：按高优/普通/响应细分更多 credit fd

做法：

- request 侧按高优、普通分别新增 credit fd
- response 侧单独一个 credit fd

优点：

- 语义更细

缺点：

- 当前单 submitter 场景收益不大
- fd 数量和测试复杂度上升

## 推荐方案

推荐 **方案 B：新增 2 个 credit eventfd**。

原因：

- 能真正去掉 request/response 两侧资源等待轮询
- 不需要按高优/普通再拆更多 fd
- 与现有框架的 `eventfd` 使用方式一致，最容易落地和维护

## 最终 fd 职责

现有 3 个 fd 保持不变：

- `highReqEventFd`
  `client -> server`
  语义：高优 request ring 从空变非空，server 可以来取请求

- `normalReqEventFd`
  `client -> server`
  语义：普通 request ring 从空变非空

- `respEventFd`
  `server -> client`
  语义：response ring 从空变非空，client 可以来取 reply/event

新增 2 个 credit fd：

- `reqCreditEventFd`
  `server -> client`
  语义：request admission 相关资源重新可用

- `respCreditEventFd`
  `client -> server`
  语义：response 发布相关资源重新可用

这 5 个 fd 分成两类：

- 新消息到达通知
- 资源恢复通知

两类语义不复用。

## 为什么不复用已有 fd

如果把 credit 信号复用到已有 fd：

- `respEventFd` 会同时表示“有响应可读”和“response 资源可写”
- `highReqEventFd` / `normalReqEventFd` 会同时表示“有请求可读”和“request 资源可写”

这样会有两个问题：

- consumer 可能读掉不属于自己的 credit 计数
- producer/consumer 很难判断本次唤醒到底意味着“有新活”还是“有新空间”

因此本设计明确要求：

- 现有 fd 只做消息到达通知
- 新增 fd 只做 credit 通知

## Request 路径设计

### 当前问题

client submitter 在以下任一条件不满足时会重试：

- `request slot` 不够
- 目标 request ring 满

当前重试方式是固定 `sleep_for(1ms)`。

### 改造后行为

submitter 尝试 admission：

1. 检查 request slot 是否可用
2. 检查目标 ring 是否可写
3. 若成功，正常写入 request slot 和 request ring
4. 若失败，阻塞等待 `reqCreditEventFd`

submitter 被 `reqCreditEventFd` 唤醒后，统一重新检查：

- request slot
- `high request ring`
- `normal request ring`

不要求 fd 精确区分是哪种资源恢复。

### 谁负责写 `reqCreditEventFd`

以下动作完成后写一次：

- server dispatcher 成功 `PopRequest()` 后
  这表示 request ring 有空间了
- client response loop 释放 request slot 后
  这表示 request slot 有空间了

### 为什么一个 `reqCreditEventFd` 就够

当前模型是：

- 单 client
- 单 submitter
- submitter 每次只会尝试一条待提交请求

因此 submitter 醒来后统一重试 admission 即可，不需要把高优/普通再拆成两条 credit fd。

## Response 路径设计

### 当前问题

server response writer 在以下任一条件不满足时会重试：

- `response slot` 不够
- `response ring` 满

当前也是固定 `sleep_for(1ms)`。

### 改造后行为

response writer 尝试发布：

1. 申请 response slot
2. 写 response ring
3. 若成功，正常写 `respEventFd`
4. 若失败，阻塞等待 `respCreditEventFd`

response writer 被唤醒后，统一重新检查：

- response slot
- response ring

### 谁负责写 `respCreditEventFd`

以下动作完成后写一次：

- client response loop 成功 `PopResponse()` 后
  这表示 response ring 有空间了
- client response loop 释放 response slot 后
  这表示 response slot 有空间了

不建议只在释放 response slot 后写，因为 response ring 和 response slot 的恢复时机不同。

## 事件语义

`eventfd` 在本设计里仍然只是 wakeup hint，不是资源计数真相。

设计原则：

- 允许多次写被合并
- 允许一次唤醒后发现多种资源都已恢复
- 允许被唤醒后实际发现条件仍不满足，然后继续等待

真正的事实来源始终是共享内存里的：

- ring cursor
- slot pool 状态

因此本设计不要求：

- 一次资源恢复严格对应一次 `eventfd write`
- `eventfd` 计数严格等于 ring/slot 的释放次数

## 数据流

### 请求方向

生产：

- 业务线程 -> submit queue
- submitter 尝试 admission
- 成功则写 request slot / request ring，并在需要时写 `highReqEventFd` 或 `normalReqEventFd`
- 失败则等待 `reqCreditEventFd`

消费：

- server dispatcher `PopRequest()`
- 成功后写 `reqCreditEventFd`
- worker 执行 handler

### 响应方向

生产：

- response writer 尝试申请 response slot / 写 response ring
- 成功则在需要时写 `respEventFd`
- 失败则等待 `respCreditEventFd`

消费：

- client response loop `PopResponse()`
- 成功后写 `respCreditEventFd`
- 释放 response slot 后再写 `respCreditEventFd`
- 若是 reply，释放 request slot 后写 `reqCreditEventFd`

## 需要修改的模块

### 公共接口

- `include/memrpc/core/bootstrap.h`
  `BootstrapHandles` 新增：
  - `reqCreditEventFd`
  - `respCreditEventFd`

### bootstrap

- `include/memrpc/client/demo_bootstrap.h`
- `src/bootstrap/posix_demo_bootstrap.cpp`

需要：

- 创建这 2 个 fd
- 在 `Connect()` / `server_handles()` 中正确 dup
- 在 reset/shutdown 时关闭

### session

- `src/core/session.h`
- `src/core/session.cpp`

需要：

- 让 session 持有并返回这 2 个 fd
- 不修改共享内存布局

### client

- `src/client/rpc_client.cpp`

需要：

- submitter 在 admission 失败时等待 `reqCreditEventFd`
- response loop 在 `PopResponse()`、释放 response slot、释放 request slot 后写 credit fd

### server

- `src/server/rpc_server.cpp`

需要：

- dispatcher 在 `PopRequest()` 后写 `reqCreditEventFd`
- response writer 在资源不足时等待 `respCreditEventFd`

### 测试

- `tests/memrpc/rpc_client_integration_test.cpp`
- `tests/memrpc/response_queue_event_test.cpp`
- `tests/memrpc/session_test.cpp`

## 错误处理

本设计不改变现有高层错误语义：

- request admission 超时仍按现有规则返回
- response 发布失败仍按现有 broken session / `PeerDisconnected` 路径处理
- Harmony 外部 death handling 假设保持不变

变化只在于：

- 等待资源恢复的方式从 `sleep_for(1ms)` 改成阻塞等 credit fd

## 风险

### 1. credit 漏写

如果某个释放路径没有写 credit fd，等待方可能长时间卡住。

因此测试必须覆盖：

- `PopRequest()` 后唤醒 submitter
- request slot 释放后唤醒 submitter
- `PopResponse()` 后唤醒 response writer
- response slot 释放后唤醒 response writer

### 2. credit 多写

多写通常不会破坏正确性，只会增加额外 syscall。

因此优先级是：

- 先保证不漏唤醒
- 再考虑是否需要进一步减少 credit write 次数

### 3. 只依赖 fd 而不重查共享状态

这是错误的。

正确做法是：

- 被唤醒后总是重新检查 ring/slot 是否真的可用

## 实施顺序

推荐按下面顺序落地：

1. 先改 `BootstrapHandles` 和 bootstrap，把 2 个新 fd 接进来
2. 先改 response 路径
   response writer 的状态机更集中，更容易先验证正确性
3. 再改 request 路径
4. 最后补文档和统计

## 结论

本设计推荐在当前协议基础上新增 2 个 credit `eventfd`：

- `reqCreditEventFd`
- `respCreditEventFd`

这样可以在不重构共享内存协议的前提下，把 request/response 两侧资源等待从定时轮询改成事件驱动，是当前最适合你们框架的工业化收敛路径。
