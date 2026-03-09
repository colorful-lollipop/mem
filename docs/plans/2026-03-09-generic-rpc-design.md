# 通用 RPC 槽位化改造设计

## 背景

当前框架的共享内存通道、优先级队列、线程池、会话恢复已经可用，但调用层仍然写死在 `Scan` 上。

这会导致每新增一个函数，例如 `ScanBehavior`，都要同时修改：

- `include/memrpc/client.h`
- `include/memrpc/handler.h`
- `src/core/protocol.h`
- `src/client/engine_client.cpp`
- `src/server/engine_server.cpp`

这种结构对“只有一个函数”的阶段是够用的，但不适合后续持续增加 RPC 能力。

本次设计目标不是把框架做成复杂泛型 RPC 系统，而是在保持共享内存模型、优先级队列和恢复逻辑不变的前提下，把“新增一个函数”的改动范围压缩到最小。

## 目标

- 保留当前共享内存 + `eventfd` + 双优先级队列 + 双线程池模型。
- 保留当前同步 OO 风格接口，外部继续像本地阻塞调用一样使用。
- 将 slot 从“写死的 `ScanRequest/ScanResult` 结构”升级为“有限通用 request/response 字节区”。
- 新增 RPC 时，不再修改共享内存布局、主调度流程和恢复逻辑。
- 第一阶段只迁移 `Scan` 到新内核，不同时引入多个新函数。

## 非目标

- 不引入 protobuf、json、TLV 之类复杂序列化框架。
- 不实现流式传输、取消、中断、分块大报文。
- 不追求极限性能，只要求简单、稳定、性能足够。
- 不改变当前 SA/bootstrap、死亡回调和 session 恢复语义。

## 方案比较

### 方案一：继续按函数写死 payload

做法：

- 每个函数一个固定 slot 结构
- client/server 主流程里继续写 `if opcode == ...`

优点：

- 改法最直接

缺点：

- 每增一个函数都要碰核心流程
- 协议和实现继续耦合

结论：

- 不采用

### 方案二：完全泛型 TLV / 反射式 RPC

做法：

- 引入通用字段系统
- 使用 TLV 或自描述消息

优点：

- 扩展性最强

缺点：

- 实现复杂度明显升高
- 不符合“代码越少越好”的约束

结论：

- 不采用

### 方案三：固定上限通用槽位 + 每个 RPC 自己编解码

做法：

- 共享内存 slot 改成通用 request/response 头 + 固定大小字节区
- client 内部增加通用 `Invoke()`
- server 增加 `opcode -> handler` 注册表
- 每个 RPC 单独写简易二进制编解码

优点：

- 改动小，结构清晰
- 不引入复杂依赖
- 后续新增函数时不再修改传输层核心

缺点：

- 每个 RPC 仍需要写少量 encode/decode 代码

结论：

- 采用本方案

## 通用协议布局

当前 `SlotPayload` 是专门为 `Scan` 定制的，路径和消息字段都写死在结构体里。改造后使用固定大小的通用槽位。

建议默认上限：

- `kDefaultMaxRequestBytes = 16 * 1024`
- `kDefaultMaxResponseBytes = 1024`

同时这两个值不写死在单个 RPC 上，而是作为 session 级统一配置存在，例如：

- `max_request_bytes`
- `max_response_bytes`

同一个 session 内所有 RPC 共用一套请求/响应大小限制。

建议结构：

```cpp
enum class Opcode : uint16_t {
  ScanFile = 1,
  ScanBehavior = 2,
};

struct RpcRequestHeader {
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint32_t flags = 0;
  uint32_t priority = 0;
  uint16_t opcode = 0;
  uint16_t reserved = 0;
  uint32_t payload_size = 0;
};

struct RpcResponseHeader {
  uint32_t status_code = 0;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  uint32_t payload_size = 0;
};

struct SlotPayload {
  RpcRequestHeader request;
  uint8_t request_bytes[max_request_bytes]{};
  RpcResponseHeader response;
  uint8_t response_bytes[max_response_bytes]{};
};
```

约束：

- 所有 RPC 都必须满足“单请求、单响应、总大小不超过 session 配置上限”
- 编码后超过上限，客户端直接返回 `StatusCode::InvalidArgument`
- 服务端解码发现越界或格式错误，返回 `StatusCode::ProtocolMismatch`
- 默认配置建议为“请求 16KB，响应 1KB”，后续如有需要可通过 session 配置整体调整

## Client 抽象

客户端新增内部通用调用入口：

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

内部方法：

```cpp
StatusCode Invoke(const RpcCall& call, RpcReply* reply);
```

职责：

- 检查 payload 大小
- 申请 slot
- 写入 request header 和 request bytes
- 进入高优或普通 ring
- 等待 response
- 解析 response header 和 response bytes

对外业务方法仍然保留：

- `EngineClient::Scan(...)`
- 后续 `EngineClient::ScanBehavior(...)`

这些方法只负责：

- 编码 request
- 调 `Invoke()`
- 解码 response

这样新增一个 RPC 时，不再改动发布等待主流程。

## Server 抽象

服务端从“固定 `HandleScan()`”改成通用分发：

```cpp
struct RpcServerCall {
  Opcode opcode;
  Priority priority;
  uint32_t queue_timeout_ms;
  uint32_t exec_timeout_ms;
  uint32_t flags;
  std::span<const uint8_t> payload;
};

struct RpcServerReply {
  StatusCode status;
  int32_t engine_code;
  int32_t detail_code;
  std::vector<uint8_t> payload;
};

using RpcHandler = std::function<void(const RpcServerCall&, RpcServerReply*)>;
```

服务端维护注册表：

```cpp
std::unordered_map<Opcode, RpcHandler> handlers;
```

worker 线程主流程只做：

1. 从 slot 读取 `RpcRequestHeader`
2. 依据 `opcode` 查找 handler
3. 调 handler
4. 把 `RpcResponseHeader + payload` 写回 slot
5. 推送 `ResponseRing`

如果未注册对应 `opcode`：

- 返回 `StatusCode::InvalidArgument`
- `message` 或 payload 中写入简单错误说明

## 编解码策略

为了保持简单，不引入通用序列化框架，只提供一组小型二进制编解码工具。

基础规则：

- `uint32_t`、`int32_t` 按固定宽度写入
- `string` 编码为 `[length:uint32][bytes]`
- 解码时必须逐步做边界检查

建议新增：

- `src/core/byte_writer.h/.cpp`
- `src/core/byte_reader.h/.cpp`

只提供少量方法：

- `WriteUint32`
- `WriteInt32`
- `WriteBytes`
- `WriteString`
- `ReadUint32`
- `ReadInt32`
- `ReadBytes`
- `ReadString`

每个 RPC 单独提供 codec，例如：

- `src/rpc/scan_codec.h/.cpp`
- `src/rpc/scan_behavior_codec.h/.cpp`

## 新增一个 RPC 时的最小改动

以 `ScanBehavior` 为例，目标改动范围是：

1. 增加类型定义
   - `ScanBehaviorRequest`
   - `ScanBehaviorResult`
2. 给 `Opcode` 增加 `ScanBehavior`
3. 增加 `scan_behavior_codec.h/.cpp`
4. `EngineClient` 增加一个薄包装方法
5. 服务端注册一个新的 handler

不再修改：

- 共享内存布局
- ring buffer
- slot 生命周期
- session 恢复
- dispatcher / worker pool 主流程

## 分阶段实施

### 第一阶段：抽通用内核，仅迁移 `Scan`

- 改造 `SlotPayload`
- 增加 `Invoke()`
- 增加服务端 handler 注册表
- 为 `Scan` 提供 codec
- 用 `Scan` 跑通现有测试和 demo

目标：

- 行为不变
- 当前功能不回退
- 架构完成解耦

### 第二阶段：新增 `ScanBehavior`

- 定义 request/response
- 写 codec
- 注册 handler
- 增加客户端薄接口
- 补测试

目标：

- 验证“新增函数时改动最小化”是否真正成立

## 风险与控制

主要风险：

- 通用 slot 改造可能破坏当前 `Scan` 行为
- 编解码边界检查遗漏可能引入协议错误
- 抽象过度会让代码变重

控制方式：

- 先只迁移 `Scan`，不同时引入多个 RPC
- 采用 TDD，先写失败测试再改代码
- 保持编解码工具极小，不做模板大抽象
- 复用现有双优先级队列、恢复逻辑和会话管理

## 结论

本次改造的核心不是“做一个泛型大框架”，而是把当前已验证可用的共享内存通道抽成一个简单可复用的 RPC 内核。

最终目标是：

- 当前 `Scan` 继续稳定工作
- 后续增加 `ScanBehavior` 等函数时，不再修改核心传输层
- 代码维持简单、低复杂度、性能足够
