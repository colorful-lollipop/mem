# ScanBehavior 轮询通知设计

## 背景

当前 `ScanBehavior` 场景和普通 `ScanFile` 不完全一样。

用户进一步明确了真实语义：

1. 每次 `ScanBehavior(accessToken, event, bundleName)` 仍然是一次独立请求
2. 它不是长会话，不是流式 RPC，也不是“一次请求多次回包”
3. 业务上可能连续发很多次 `ScanBehavior`
4. 除了本次调用的同步返回值，还可能产生额外通知
5. 这些额外通知最终需要在主进程本地回调给 listener

这意味着当前的核心问题不是“如何让一个请求多次返回”，而是：

- 如何在不破坏 `MemRpc` 简洁 request/response 模型的前提下，
- 兼容 `ScanBehavior` 的额外通知语义，
- 同时尽量保持对原有 `VirusEngineManager` 接口的兼容性。

## 设计结论

本次场景采用以下模型：

1. `ScanBehavior` 继续作为普通 RPC
2. 额外通知不走独立 `notify ring + notify_eventfd`
3. 额外通知不混入普通响应队列协议
4. 子进程只维护本地 `BehaviorScanResult` 队列
5. 主进程通过普通 `PollBehaviorReports()` RPC 主动拉取这些通知
6. 主进程收到后，在本地广播给所有 listener

简化表达就是：

- `ScanBehavior()` 负责同步返回本次调用结果
- `PollBehaviorReports()` 负责异步通知搬运

## 为什么不用“一次请求多次回包”

这种做法会直接破坏当前共享内存框架的核心模型：

- 一个 `request_id` 对应一个等待上下文
- 一个请求对应一个结果
- 响应队列只承担“已请求事务的回包”

如果把 `ScanBehavior` 扩展成“一次请求多次回包”：

- pending request 生命周期会变复杂
- 超时语义会变复杂
- session 恢复会变复杂
- 同步兼容层会很难写清楚

这和当前“核心通信层高性能、低复杂度”的目标不一致。

## 为什么不用独立 notify 通道

独立 `notify ring + notify_eventfd` 在技术上可行，但不是当前最优解。

原因：

1. 用户已经明确当前没有“子进程必须主动推送”的刚性场景
2. 独立事件通道会额外引入一条共享内存 ring、一组 fd 和一套状态机
3. `ScanBehavior` 的额外通知可以接受由主进程主动轮询获取
4. 轮询方案已经足够兼容现有 listener 语义

因此当前主线应收敛回：

- 高优请求队列
- 普通请求队列
- 响应队列
- 一个响应 `eventfd`

## 语义模型

### 同步路径

每次调用：

```cpp
int32_t ScanBehavior(uint32_t accessToken,
    const std::string& event,
    const std::string& bundleName);
```

语义保持不变：

- 主进程发起普通 RPC
- 子进程执行一次行为分析
- 同步返回 `int32_t` 状态码

### 通知路径

如果本次行为分析触发了额外通知：

- 子进程将一个 `BehaviorScanResult` 放入本地待上报队列
- 主进程有 listener 时，后台轮询线程定期调用 `PollBehaviorReports()`
- 子进程从队列中批量取出若干条通知并通过普通响应返回
- 主进程逐条广播给本地 listener

### listener 模型

listener 继续只存在于主进程。

子进程不感知：

- `std::shared_ptr<ScanResultListener>`
- listener 数量
- listener 生命周期

子进程只关心：

- 当前是否可能产生行为通知
- 本地待上报通知队列里还有哪些 `BehaviorScanResult`

## 主进程职责

主进程 `VirusEngineManager` 或其内部辅助对象负责：

1. 管理本地 listener 集合
2. 当 listener 数量从 `0 -> 1` 时启动轮询线程
3. 轮询线程周期性调用 `PollBehaviorReports()`
4. 收到通知后本地广播给所有 listener
5. 当 listener 数量从 `1 -> 0` 时停止轮询线程
6. 在 session 失效或子进程重启后继续通过普通 RPC 恢复轮询

主进程不再承担：

- 子进程 signal 级兜底
- 跨进程 listener 对象传递
- 独立事件通道维护

## 子进程职责

子进程 `VirusEngineService` 负责：

1. 正常执行 `ScanBehavior`
2. 在需要时生成 `BehaviorScanResult`
3. 将这些结果追加到本地待拉取队列
4. 提供 `PollBehaviorReports()` RPC
5. 在 `PollBehaviorReports()` 中批量返回队列中的通知

这条路径和 `ScanBehavior()` 同样都是普通 request/response RPC。

## `PollBehaviorReports()` 设计

建议采用批量拉取语义，而不是一次只返回一条。

### 请求

第一版可以很简单：

- 空请求，或只带 `max_items`

建议默认批量上限较小，例如：

- `8`
- 或 `16`

### 响应

```cpp
struct PollBehaviorReportsReply {
    int32_t resultCode = SUCCESS;
    std::vector<BehaviorScanResult> reports;
};
```

语义如下：

- `SUCCESS + empty reports`
  - 当前没有新通知
- `SUCCESS + non-empty reports`
  - 返回一批通知
- 其他错误码
  - 本次轮询失败，主进程稍后继续重试

## 为什么不把“特殊通知结果”直接塞进响应队列协议

用户提出过一个方向：

- 是否让响应队列里出现“特殊结果”，由主进程特殊处理

这个方向不适合作为框架主线，原因是：

1. 会把业务通知语义混入通用传输层
2. 会让 `MemRpc` 的 response dispatcher 需要理解 VPS 业务类型
3. 后面新增其他应用时也会倾向继续往框架里塞特殊分支
4. 会削弱框架的通用性和优雅性

更好的做法是：

- 框架保持纯 request/response
- 应用层显式定义一个 `PollBehaviorReports()` RPC
- 通过普通响应返回业务通知列表

这样边界最清楚。

## 对原接口兼容性的影响

### `ScanBehavior`

对外接口不变：

- 继续返回 `int32_t`
- 继续表示这次行为分析调用的直接结果

### `RegisterScanResultListener`

接口形状不变，但内部语义调整为：

- 只注册到主进程本地容器
- 启动或维持后台轮询线程

### `UnRegisterScanResultListener`

接口形状不变，但内部语义调整为：

- 从主进程本地容器移除
- 必要时停止后台轮询线程

因此旧业务代码的感知仍然是：

- 调 `ScanBehavior()`
- 注册 listener
- 稍后收到回调

只是内部从“子进程主动回调”变成“主进程主动拉取后本地回调”。

## 对框架的影响

本方案要求框架做一件减法：

- 移除独立 `notify ring + notify_eventfd` 主线实现

框架保留的核心模型重新收敛为：

1. 高优请求队列
2. 普通请求队列
3. 响应队列
4. 一个响应 `eventfd`

如果未来确实出现必须由子进程主动推送的场景，再单独评估是否需要重引入独立事件通道。

## 测试重点

需要重点覆盖：

1. `ScanBehavior()` 仍然是普通同步 RPC
2. `PollBehaviorReports()` 能批量拉取通知
3. 没有 listener 时不启动轮询线程
4. 有 listener 时，主进程能把拉取结果广播给本地 listener
5. 子进程重启后，轮询线程能继续通过普通 RPC 恢复工作
6. 删除 notify 通道后，核心 RPC 路径和 `MiniRpc` 仍然稳定

## 结论

本次 `ScanBehavior` 场景不应推动框架走向“一次请求多次回包”或“内建特殊通知帧”。

当前最简洁、兼容且优雅的方案是：

- `ScanBehavior()` 保持普通 RPC
- 额外通知通过 `PollBehaviorReports()` 普通 RPC 拉取
- listener 仅存在于主进程
- 框架继续保持纯 request/response 共享内存模型
