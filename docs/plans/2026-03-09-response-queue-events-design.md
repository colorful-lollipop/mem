# 单响应队列事件模型设计

## 背景

当前框架已经明确了两条长期边界：

1. `MemRpc` 只做通用共享内存 IPC 框架
2. 应用层自己兼容自己的旧接口，不把业务兼容层塞回框架

在此基础上，`ScanBehavior` 又提出了一个新要求：

- 每次 `ScanBehavior(accessToken, event, bundleName)` 仍然是一次普通独立请求
- 但子进程除了本次 RPC 的同步返回外，还可能产生额外通知
- 这些通知最终需要回到主进程，再由主进程自己的 listener 体系分发

用户进一步确认了一个关键边界：

- 这些额外通知是“无头事件”
- 不需要绑定到某一次具体请求
- 只需要主进程能够收到，然后交给应用层自己分发

这使得“共用一个响应队列承载普通回包和额外通知”成为可行方案。

## 设计结论

采用单响应队列双消息类型模型：

1. 保留现有请求队列和响应队列
2. 保留现有 `resp_eventfd`
3. 不再引入独立 `notify ring + notify_eventfd`
4. 响应队列中的消息分为两类：
   - `Reply`
   - `Event`
5. `Reply` 用于普通 RPC 回包
6. `Event` 用于子进程主动投递的无头额外通知

简化表达：

- 请求还是普通请求
- 响应队列既能回包，也能发事件
- 所有通知都共用一个响应唤醒 fd

## 为什么这版优于轮询

之前讨论过 `PollBehaviorReports()` 轮询模型，它的优点是边界保守，但也有明显代价：

1. 主进程需要后台轮询
2. 空闲时有额外 wakeup 和 RPC 开销
3. 通知延迟取决于轮询周期
4. listener 实现会多出一层轮询线程控制

现在既然已经确认：

- 额外通知不要求绑定具体请求
- 主进程可以接受统一事件分发

那么直接把它们放进响应队列更自然：

- 不需要轮询
- 不需要第二条事件通道
- 只复用现有响应分发线程

## 为什么不恢复独立 notify 通道

独立事件通道的问题不在于做不到，而在于当前不划算。

它会引入：

- 新的 ring
- 新的 `eventfd`
- 新的布局参数
- 新的会话初始化逻辑
- 新的测试矩阵

而这次事件模型其实并不需要这么重。

当前事件只是：

- 子进程偶尔发来一条“无头广播事件”

因此独立通道属于过度设计。

## 为什么不会破坏框架优雅性

关键点在于：框架只区分“消息种类”，不理解业务内容。

框架层只知道：

- 这是一个 `Reply`
- 这是一个 `Event`
- 这个 `Event` 属于哪个 `event_domain/event_type`

框架层不知道：

- `BehaviorScanResult`
- `ScanResultListener`
- `VirusEngineManager`

业务解释仍然完全留在应用层。

所以这不是把业务协议塞进框架，而是给响应队列增加一个很薄的通用消息头。

## 协议模型

### 响应消息种类

新增枚举：

```cpp
enum class ResponseMessageKind : uint16_t {
    Reply = 0,
    Event = 1,
};
```

### 响应队列 entry

当前响应队列 entry 在语义上主要承载：

- `request_id`
- `slot_index`
- `status_code`
- `payload_size`

建议扩展为：

- `message_kind`
- `request_id`
- `slot_index`
- `status_code`
- `event_domain`
- `event_type`
- `flags`
- `payload_size`

其中：

- `Reply`
  - 使用 `request_id`
  - 使用 `status_code`
  - `event_domain/event_type` 忽略
- `Event`
  - `request_id = 0`
  - `status_code` 可忽略或保留为 `Ok`
  - 使用 `event_domain/event_type`

### slot payload

不需要拆两套共享内存结构。

继续复用统一 slot payload：

- `Reply` 时放响应 payload
- `Event` 时放事件 payload

这样改动最小：

- 不新增 slot 类型
- 不新增共享内存区域
- 不新增独立 ring

## 事件快速识别字段

用户明确希望事件能快速识别类型，避免先做完整反序列化。

因此建议在 `Event` entry 中预留：

- `event_domain`
- `event_type`
- `flags`

这样主进程 dispatcher 能在 O(1) 时间内先完成分流，再决定是否需要反序列化 payload。

### 为什么需要 `event_domain`

后续框架可能同时服务多个应用层：

- `MiniRpc`
- `Vps`
- 未来其他 IPC 应用

如果只有 `event_type`，不同应用层很容易冲突。

加入 `event_domain` 后：

- 框架先按 domain 粗分
- 应用层再按 type 细分

这能让事件系统保持通用。

## 主进程行为

主进程 `RpcClient` 的响应分发线程继续只监听一个 `resp_eventfd`。

每次被唤醒后：

1. 批量 drain 响应队列
2. 对每个 entry 先看 `message_kind`

处理规则：

### `Reply`

- 走现有逻辑
- 通过 `request_id` 找到 pending request
- 填充 `RpcReply`
- 唤醒等待者

### `Event`

- 不查 pending map
- 把 `event_domain/event_type/payload` 投递给事件回调
- 由应用层自行解码和分发

这意味着框架只多出一个非常薄的分支。

## 子进程行为

`RpcServer` 除了写普通回包外，还允许主动写一条 `Event` 到响应队列。

流程：

1. 子进程决定发布事件
2. 申请一个 slot
3. 写入事件 payload
4. 在响应队列中写入一条 `message_kind = Event` 的 entry
5. `write(resp_eventfd, 1)`

和普通回包共用同一套 wakeup 机制。

## 框架接口

建议框架提供通用事件接口，而不是业务专用接口。

### 客户端

```cpp
struct RpcEvent {
    uint16_t domain = 0;
    uint16_t type = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> payload;
};

using RpcEventCallback = std::function<void(const RpcEvent&)>;
```

客户端暴露：

```cpp
void SetEventCallback(RpcEventCallback callback);
```

### 服务端

服务端暴露：

```cpp
StatusCode PublishEvent(const RpcEvent& event);
```

框架层只搬运这个通用事件对象，不负责解释 payload。

## 对 `ScanBehavior` 的影响

`ScanBehavior()` 本身仍然保持普通 RPC：

- 有自己的 request
- 有自己的同步返回值
- 不变成长事务

如果行为扫描额外产生一条业务通知：

- 子进程发布一条 `RpcEvent`
- `event_domain = Vps`
- `event_type = BehaviorReport`
- payload 编码 `BehaviorScanResult`

主进程收到后：

- VPS 兼容层看到 `domain/type`
- 解码 `BehaviorScanResult`
- 广播给本地 listener

也就是说：

- 同步返回值仍代表本次 `ScanBehavior()` 的直接结果
- 额外通知只是附加广播事件

## 对 `MiniRpc` 的影响

`MiniRpc` 当前不需要使用事件能力。

它仍然只保留：

- `Echo`
- `Add`
- `Sleep`

这样可以继续让 `MiniRpc` 保持最小。

框架支持事件，不等于每个应用都必须使用事件。

## 对 VPS 兼容层的影响

VPS 兼容层后续可以这样落：

1. 主进程维护真实 listener 集合
2. 注册一个框架级 `SetEventCallback()`
3. 收到 `Vps/BehaviorReport` 事件后解码
4. 在主进程本地广播给 listener

这样：

- 子进程不持有 listener 对象
- 不需要轮询线程
- 不需要 `PollBehaviorReports()`

## 错误与恢复边界

事件是无头广播，不绑定某个具体请求，因此建议边界定死：

1. 不保证事件和某次具体 RPC 的强因果绑定
2. 不保证事件和普通回包的全局顺序约束超出“写入响应队列的顺序”
3. session 重建期间丢失的无头事件不做透明补发
4. 如果业务需要强一致结果，仍应走普通 RPC

这样才能保持框架简单。

## 测试重点

需要覆盖：

1. 普通 `Reply` 路径不回归
2. `Event` 能通过响应队列到达客户端回调
3. `Reply` 和 `Event` 混用时能被正确区分
4. 高优请求队列与响应事件混用时不破坏回包
5. `MiniRpc` 不使用事件时仍稳定
6. VPS 的 `ScanBehavior` 事件能在主进程本地广播

## 结论

在“额外通知是无头广播事件”的前提下，当前最优方案是：

- 不做轮询
- 不做独立 notify 通道
- 只保留一个响应队列和一个响应 `eventfd`
- 用 `Reply/Event` 双消息类型统一承载回包和事件

这版同时满足：

- 框架简洁
- 性能较好
- 边界清晰
- 对原 listener 模型兼容
