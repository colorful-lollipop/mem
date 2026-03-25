# MemRPC 与 VesClient 源码导读

本文档专门面向“要读 C++ 实现的人”，重点不是再讲一遍架构名词，而是回答两个更实际的问题：

1. 如果我要读 `memrpc`，应该先看哪些函数，按什么顺序看。
2. 如果我要读 `VesClient`，一次调用到底会经过哪些函数和对象。

配套文档：

- [architecture.md](architecture.md)
- [testing_guide.md](testing_guide.md)

## 1. 读 `memrpc` 时建议先抓哪几个文件

如果只看最关键的几处，建议按这个顺序：

1. [`memrpc/include/memrpc/core/protocol.h`](/root/code/demo/mem/memrpc/include/memrpc/core/protocol.h)
2. [`memrpc/src/core/session.cpp`](/root/code/demo/mem/memrpc/src/core/session.cpp)
3. [`memrpc/include/memrpc/client/rpc_client.h`](/root/code/demo/mem/memrpc/include/memrpc/client/rpc_client.h)
4. [`memrpc/src/client/rpc_client.cpp`](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp)
5. [`memrpc/include/memrpc/server/rpc_server.h`](/root/code/demo/mem/memrpc/include/memrpc/server/rpc_server.h)
6. [`memrpc/src/server/rpc_server.cpp`](/root/code/demo/mem/memrpc/src/server/rpc_server.cpp)
7. [`memrpc/include/memrpc/client/typed_invoker.h`](/root/code/demo/mem/memrpc/include/memrpc/client/typed_invoker.h)

这个顺序的目的很简单：

- 先知道共享内存里数据长什么样。
- 再知道 session 怎么 attach 和 push/pop。
- 再看 client/server 怎样把这些 entry 组织成一次 RPC。
- 最后再看 typed 封装怎样把业务结构体接进来。

## 2. `protocol.h` 应该看什么

[`protocol.h`](/root/code/demo/mem/memrpc/include/memrpc/core/protocol.h) 里最重要的不是某一个字段，而是三件事：

- `PROTOCOL_VERSION`
- `RequestRingEntry`
- `ResponseRingEntry`

它们决定了整个框架的硬边界：

- request/response 都是固定大小 entry
- payload 是 inline 的，不存在额外 slot 池
- client/server 必须对布局完全一致

读这份头文件时，建议重点想清楚这两个问题：

- 一个请求从业务对象变成字节后，最终落在哪个字段里。
- 一个响应或事件返回时，客户端是靠什么字段区分它属于谁。

答案分别是：

- 请求 payload 落在 `RequestRingEntry::payload`
- 返回路径主要依赖 `requestId`，事件则额外依赖 `messageKind` / `eventDomain` / `eventType`

## 3. `Session` 应该怎么读

看 [`memrpc/src/core/session.cpp`](/root/code/demo/mem/memrpc/src/core/session.cpp)。

### 3.1 先看 `Attach()`

这是理解底层共享内存接入的入口。

建议顺着下面的调用关系看：

- `Session::Attach()`
- `MapAndValidateHeader()`
- `RemapWithActualLayout()`
- `TryAcquireClientAttachment()`

读这组函数时，要抓住三个实现点：

- 为什么先 mmap header，再 remap 全布局。
- 为什么 client attach 要额外检查 `activeClientPid`。
- 为什么共享内存 mutex 会专门处理 owner-dead 和 timeout。

这三点合起来，就是 `Session` 的核心价值：

- 不是只把 shm 映射进来
- 而是尽量把“坏 session”“陈旧 client”“对端异常退出”转成可判断的框架状态

### 3.2 再看 ring 读写函数

建议直接看：

- `PushRingEntry()`
- `PopRingEntry()`
- `Session::PushRequest()`
- `Session::PopRequest()`
- `Session::PushResponse()`
- `Session::PopResponse()`

这里真正重要的是两层抽象：

- 模板函数负责通用 ring 算法
- `Session` 成员函数负责把“high request / normal request / response”映射到正确的 ring 上

所以如果你在排查：

- `QueueFull`
- ring capacity
- response credit
- eventfd 唤醒异常

通常应该优先回到 `Session` 和 `protocol.h`，而不是先看业务层。

## 4. `RpcClient` 的推荐阅读顺序

看 [`memrpc/include/memrpc/client/rpc_client.h`](/root/code/demo/mem/memrpc/include/memrpc/client/rpc_client.h) 和 [`memrpc/src/client/rpc_client.cpp`](/root/code/demo/mem/memrpc/src/client/rpc_client.cpp)。

最值得优先读的公开接口是：

- `Init()`
- `InvokeAsync()`
- `InvokeWithRecovery()`
- `SetRecoveryPolicy()`
- `RequestExternalRecovery()`
- `GetRecoveryRuntimeSnapshot()`
- `Shutdown()`

### 4.1 `Init()`

`RpcClient::Init()` 的职责不是“做一个轻量初始化”，而是把 client 进入可运行状态。

建议顺着实现重点看：

- 是否已经 `Closed`
- 是否已经 running
- `EnsureLiveSession` 是否成功
- 失败时为什么会转而 `Shutdown()`

读完后应该建立一个明确认识：

- `Init()` 不只是设置几个字段
- 它会真正尝试拿到 live session，并把 client 的后台运行条件准备好

### 4.2 `InvokeAsync()`

这是框架级一等入口。

读这个函数时，建议重点抓下面几个问题：

- 调用一开始会先检查哪些 client 状态。
- 什么情况下会直接返回 ready future。
- 请求什么时候真正获得 `requestId`。
- 请求是在哪一步写入 request ring 的。
- 写入成功后，future 是靠什么在后续完成的。

正确的阅读方式不是只盯着 `InvokeAsync()` 一小段代码，而是连着下面这些内部概念一起看：

- `PendingSubmit`
- `PendingRequest`
- response 分发线程
- timeout/watchdog 逻辑

也就是说，`InvokeAsync()` 只是“发起调用”的入口，真正让它完整成立的是整个 pending 生命周期。

### 4.3 `InvokeWithRecovery()`

这是业务 facade 很容易依赖，但也很容易误解的函数。

它做的事情是：

- 在当前调用还没有稳定进入可用 session 时
- 对 `CooldownActive` / `PeerDisconnected` 这一类恢复窗口状态做等待和重试

它不做的事情更重要：

- 不会透明重放已经成功发布到旧 session 的请求
- 不会篡改业务 opcode 或 payload

所以如果你在读业务 client 代码，看到它包了一个 `InvokeWithRecovery()`，要立刻知道它的语义是：

- “等框架恢复好再发起新的 invoke”
- 不是“帮你重试一切失败请求”

### 4.4 `SetRecoveryPolicy()` 和 `RequestExternalRecovery()`

这两个函数体现的是 `RpcClient` 的恢复边界。

- `SetRecoveryPolicy()`
  让业务层只提供策略，不直接操纵内部状态机。
- `RequestExternalRecovery()`
  让 bootstrap health check 或其他外部信号可以把恢复请求送回 `RpcClient`。

如果你在排查恢复问题，建议把这两个入口和下面几类状态一起看：

- `ClientLifecycleState`
- `RecoveryTrigger`
- `RecoveryAction`
- `RecoveryRuntimeSnapshot`
- `RecoveryEventReport`

### 4.5 `Shutdown()`

`Shutdown()` 的阅读重点是“终态语义”。

这段代码要回答的是：

- 一旦进入 `Closed`，后续还能不能自动恢复。
- 正在等待的 future 如何收尾。
- 后台线程和 session 资源如何停止。

当前设计里，`Closed` 是真正终态。读这段代码时要特别警惕一个误区：

- 业务层不应该把 `Shutdown()` 当成“短暂关闭一下，之后继续自动恢复”

## 5. `RpcClient` 一次调用的函数级阅读路径

如果你只想追一条最典型的主路径，建议按这个顺序跳转源码：

1. `RpcClient::InvokeAsync()`
2. client 内部提交/排队逻辑
3. request ring publish
4. response loop 对 `requestId` 的匹配
5. `RpcFuture::Wait()` 或 `WaitAndTake()`

如果你想追恢复路径，再加：

6. `RpcClient::InvokeWithRecovery()`
7. `RequestExternalRecovery()`
8. 生命周期转换和 snapshot/report 生成逻辑

## 6. `typed_invoker.h` 在整个设计里的位置

看 [`memrpc/include/memrpc/client/typed_invoker.h`](/root/code/demo/mem/memrpc/include/memrpc/client/typed_invoker.h)。

这份头文件特别适合拿来理解“typed API 到底是怎么套在底层 RPC 上的”。

建议依次看：

- `InvokeTyped()`
- `WaitAndDecode()`
- `Then()`
- `InvokeTypedAsync()`
- `InvokeTypedSync()`

它们分别把下面几步拆开了：

- request encode
- 构造 `RpcCall`
- 调用 `RpcClient`
- 等待 future
- decode reply

这也是为什么业务侧代码通常不需要直接接触 `std::vector<uint8_t>`：

- typed 层已经把“业务对象 <-> payload”这段样板逻辑统一了

## 7. `VesClient` 的推荐阅读顺序

看：

- [`virus_executor_service/include/client/ves_client.h`](/root/code/demo/mem/virus_executor_service/include/client/ves_client.h)
- [`virus_executor_service/src/client/ves_client.cpp`](/root/code/demo/mem/virus_executor_service/src/client/ves_client.cpp)
- [`virus_executor_service/include/transport/ves_control_interface.h`](/root/code/demo/mem/virus_executor_service/include/transport/ves_control_interface.h)

建议优先读这些函数：

- `BuildRecoveryPolicy()`
- `BuildControlLoader()`
- `Connect()`
- `Init()`
- `CurrentControl()`
- `InvokeApi()`
- `ScanFile()`
- `Shutdown()`

## 8. `VesClient` 每个关键函数该怎么理解

### 8.1 `BuildRecoveryPolicy()`

这是 `VesClient` 把业务配置翻译成 `RpcClient` 恢复策略的地方。

它的意义不是“实现恢复”，而是：

- 约定哪些失败应该触发 restart
- restart 前延迟多久
- idle 多久后触发 idle close

所以这里体现的是 policy 装配，而不是状态机本身。状态机仍然在 `RpcClient`。

### 8.2 `BuildControlLoader()`

这个函数决定了控制面 proxy 是怎么来的。

建议重点看：

- 首次是否优先使用已有 `remote`
- 后续如何通过 SA manager 重取 remote
- 为什么在重新 load 前会先 `CloseSession()` 并 sleep 一下

它本质上是把“怎么拿到 control proxy”抽象成一个 loader，方便 `VesClient` 在后续按需重用。

### 8.3 `Connect()`

`Connect()` 是面向调用者的便捷入口。

它做的事情很直接：

1. 构造 `VesClient`
2. 调 `Init()`
3. 初始化失败则返回 `nullptr`

所以如果你想排查“为什么 connect 失败”，通常不该只盯着 `Connect()`，而要继续下钻：

- `BuildControlLoader()`
- `Init()`
- `client_.Init()`

### 8.4 `Init()`

`VesClient::Init()` 是最重要的生命周期入口。

建议按下面顺序看：

1. 创建 `VesBootstrapChannel`
2. `client_.SetBootstrapChannel()`
3. 安装 health snapshot callback
4. `client_.SetRecoveryPolicy()`
5. `client_.Init()`
6. 尝试缓存当前 control 到 `fallbackControl_`

读这段代码时要明确：

- `VesClient` 自己不打开共享内存 session
- 真正的 session 初始化仍然由 `RpcClient` 完成
- `VesClient` 负责的是“把 VPS 所需的 bootstrap 和恢复配置装进去”

### 8.5 `CurrentControl()`

这个函数是理解控制面兜底路径的关键。

它的优先级是：

1. 优先向 `bootstrapChannel_` 取当前 control
2. 拿不到时退回 `fallbackControl_`
3. 再不行就调用 `controlLoader_()` 重建

这意味着 `VesClient` 处理控制面的思路不是“每次 AnyCall 现连一次”，而是：

- 优先复用当前有效 control
- 失效时平滑退回缓存
- 必要时再显式重建

### 8.6 `InvokeApi()`

这是整个 `VesClient` 最关键的函数。

建议按下面顺序读源码：

1. 空指针检查
2. 计算 `minRecoveryWaitMs`
3. 进入 `client_.InvokeWithRecovery()`
4. `EncodeMessage<Request>()`
5. 判断 payload 是否超限
6. 小包走 `memrpc`
7. 大包走 `AnyCall`
8. decode typed reply

这里最重要的设计判断是：

- `VesClient` 按“请求是否适合进入 `memrpc`”来选路
- 不是按“这次调用失败了就自动切控制面”来选路

所以它不是失败补救器，而是主路径/旁路选择器。

### 8.7 `ScanFile()`

`ScanFile()` 本身非常薄，它的价值不在函数体复杂，而在于它展示了业务 facade 的标准写法：

- 业务方法只负责把 typed 请求交给 `InvokeApi()`
- opcode、priority、timeout 作为 facade 参数进入
- 具体 encode/decode、主路径/旁路选择、恢复等待都交给底层通用逻辑

如果后续要新增新的 typed 业务接口，最应该参考的不是 transport 层，而是 `ScanFile()` 这种 facade 形态。

### 8.8 `Shutdown()`

`VesClient::Shutdown()` 做了两件事：

- 清空恢复策略
- 调用 `client_.Shutdown()`
- 释放 `bootstrapChannel_`

这段代码体现的是一个很重要的收尾原则：

- 业务 facade 的关闭，不应该留下还在运行的恢复逻辑

## 9. 一次 `VesClient::ScanFile()` 的函数级调用路径

可以把主路径理解成下面这条链：

1. `VesClient::ScanFile()`
2. `VesClient::InvokeApi()`
3. `client_.InvokeWithRecovery()`
4. `MemRpc::EncodeMessage<ScanTask>()`
5. 小包时进入 `MemRpc::InvokeTypedSync()` / `RpcClient::InvokeAsync()`
6. 服务端 `RpcServer` 消费请求
7. `VesEngineService::ScanFile()` 执行业务逻辑
8. 响应写回 response ring
9. client 侧 decode 成 `ScanFileReply`

如果请求太大，则第 5 步改成：

- 构造 `VesAnyCallRequest`
- `CurrentControl()`
- `IVirusProtectionExecutor::AnyCall()`
- decode `VesAnyCallReply`

## 10. 读这份源码导读时应该配合哪些测试

如果你在读 `RpcClient`，建议配合看：

- [`memrpc/tests/rpc_client_timeout_watchdog_test.cpp`](/root/code/demo/mem/memrpc/tests/rpc_client_timeout_watchdog_test.cpp)
- [`memrpc/tests/engine_death_handler_test.cpp`](/root/code/demo/mem/memrpc/tests/engine_death_handler_test.cpp)

如果你在读 `VesClient`，建议配合看：

- [`virus_executor_service/tests/dt/ves_crash_recovery_test.cpp`](/root/code/demo/mem/virus_executor_service/tests/dt/ves_crash_recovery_test.cpp)
- [`virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp`](/root/code/demo/mem/virus_executor_service/tests/unit/testkit/testkit_async_pipeline_test.cpp)

原因很直接：

- 单看实现，你能知道代码怎么写
- 配合这些测试，你才能知道这些函数在真实恢复、超时、旁路和吞吐场景下是怎么被触发的
