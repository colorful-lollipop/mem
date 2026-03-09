# 异步内核与应用层解耦设计

## 背景

当前框架已经具备：

- 共享内存 + `eventfd` 通信
- 双优先级请求队列
- 通用 request/response 槽位
- `opcode -> handler` 服务端分发
- 同步 `Scan()` / `ScanBehavior()` 兼容接口

但目前还有两个明显问题：

1. 框架客户端接口仍然偏应用化，公共能力不足  
2. 应用层 demo 容易反向牵引框架结构，导致通用性下降

用户明确的新目标是：

- 框架层必须通用
- 应用层只是一个示例，方便后续迁移
- 底层按异步模型设计
- 再向上兼容旧的同步接口
- 重点仍然是框架本身的易用性、性能和稳定性

## 核心结论

共享内存框架的底层事务模型应该按“异步调用”设计。

也就是说：

- 请求发布是异步的
- 结果返回是异步完成的
- 超时、子进程异常、事件通知本质上都是异步状态变化

同步接口不应作为框架底层原语，而应作为：

- `InvokeAsync()` 之上的兼容包装

最终分层应为：

1. 框架内核层
2. 框架客户端包装层
3. 应用层兼容示例

## 分层结构

### 1. 框架内核层

只负责通用能力，不出现应用语义：

- session 管理
- slot 生命周期
- request/response ring
- 异步请求表
- `request_id`
- 超时
- 子进程异常恢复
- 异步事件队列
- 服务端 handler 注册

这一层不应包含：

- `VirusEngineManager`
- `ScanTask`
- `IVirusEngine`
- 业务 listener 类型

### 2. 框架客户端包装层

对外提供两类接口：

- `InvokeAsync()`
- `InvokeSync()`

其中：

- `InvokeAsync()` 是一等接口
- `InvokeSync()` 基于 `InvokeAsync() + 等待完成` 实现

这层仍然不感知业务类型，只处理：

- `Opcode`
- `RpcCall`
- `RpcReply`
- future / promise / completion handle

### 3. 应用层

应用层是框架使用者，不再主导框架结构。

例如：

- `VirusEngineManager`
- `VirusEngineService`
- fake engine
- `ScanTask/ScanResult` codec

它们都应该建立在框架公共异步/同步接口之上。

## 建议的客户端公共 API

建议新增公共接口：

```cpp
struct RpcCall {
  Opcode opcode;
  Priority priority;
  uint32_t queue_timeout_ms;
  uint32_t exec_timeout_ms;
  uint32_t flags;
  std::vector<uint8_t> payload;
};

struct RpcReply {
  StatusCode status;
  int32_t engine_code;
  int32_t detail_code;
  std::vector<uint8_t> payload;
};
```

异步句柄建议保持简单，不引入复杂调度库：

```cpp
class RpcFuture {
 public:
  StatusCode Wait(RpcReply* reply);
  bool IsReady() const;
};
```

客户端建议提供：

```cpp
class RpcClient {
 public:
  StatusCode Init();
  RpcFuture InvokeAsync(const RpcCall& call);
  StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);
  void Shutdown();
};
```

这里的 `InvokeSync()` 只是：

- `InvokeAsync()`
- 然后等待 future 完成

## 为什么不直接只保留同步接口

如果继续把同步接口当内核模型，会导致这些能力都变得别扭：

- 多并发请求
- 取消
- 超时分层
- 异步事件通知
- 子进程异常导致的批量失败
- 后续更高层的任务调度

而底层先按异步建模后：

- 同步只是外层包装
- 事件和异常路径天然统一
- 应用层保留旧接口也更容易

## 服务端公共 API

服务端也应进一步收敛为通用接口：

```cpp
using RpcHandler = std::function<void(const RpcServerCall&, RpcServerReply*)>;

class RpcServer {
 public:
  void RegisterHandler(Opcode opcode, RpcHandler handler);
  StatusCode Start();
  void Stop();
};
```

`EngineServer` 可以逐步退化成：

- 兼容旧名字的薄封装

而真正通用的名字应是 `RpcServer`。

## 应用层如何兼容旧同步接口

以 `VirusEngineManager` 为例：

- `ScanFile(...)`
  - 编码 `ScanTask`
  - 调 `InvokeSync()`
  - 解码 `ScanResult`
- `ScanBehavior(...)`
  - 编码行为请求
  - 调 `InvokeSync()`
  - 取整型返回值

如果未来应用层也要暴露异步能力，可以再新增：

- `ScanFileAsync(...)`
- `ScanBehaviorAsync(...)`

但这属于应用层选择，不影响框架公共接口。

## 异步事件模型

框架除了同步/异步调用，还需要公共异步事件能力。

建议单独抽象：

- 事件 ring
- 事件 dispatcher
- 事件订阅回调

这样 `BehaviorScanResult` 广播只是一个应用例子，而不是框架唯一用途。

也就是说：

- 框架只提供“发布事件 / 消费事件”
- 应用层决定把事件解释成 listener 广播

## 异常恢复语义

异步模型下，子进程异常处理更自然：

- 所有 pending future 立即失败
- 事件流停止
- 当前 session 作废
- 下次请求触发恢复

这比“同步调用里再额外嵌 signal 处理”清晰得多。

因此，原来应用层里的 signal 兜底逻辑应该删除，并由框架统一接管。

## 对现有代码的影响

本次调整不要求推翻已有实现，而是逐步收敛：

### 保留

- 现有共享内存布局
- 现有双优先级队列
- 现有恢复逻辑
- 现有 `opcode -> handler`

### 新增

- 公共 `RpcClient`
- 公共 `RpcFuture`
- 公共 `RpcServer`
- 公共事件抽象

### 迁移

- `EngineClient` 逐步变成 `RpcClient` 的兼容封装
- `EngineServer` 逐步变成 `RpcServer` 的兼容封装
- `VirusEngineManager` 改为纯应用层示例

## 结论

正确方向不是“同步共享内存调用”，而是：

- 底层异步
- 外层同步兼容
- 框架通用
- 应用层示例化

这能同时满足：

- 对旧接口的兼容
- 对新场景的扩展
- 框架层的解耦
- 后续性能与稳定性优化空间
