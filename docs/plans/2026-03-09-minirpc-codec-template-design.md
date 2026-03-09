# MiniRpc Codec 模板化设计

## 背景

当前 MiniRpc codec 暴露的是按消息名展开的函数：

- `EncodeEchoRequest()`
- `DecodeEchoRequest()`
- `EncodeEchoReply()`
- `DecodeEchoReply()`
- `EncodeAddRequest()`
- `DecodeAddRequest()`
- `EncodeAddReply()`
- `DecodeAddReply()`
- `EncodeSleepRequest()`
- `DecodeSleepRequest()`
- `EncodeSleepReply()`
- `DecodeSleepReply()`

这套接口在类型较少时可接受，但随着消息类型增加，会带来两个问题：

1. 调用点必须依赖一组不断增长的显式函数名
2. 新增消息类型时，编解码扩展点不统一

用户明确要求将 codec 按类型模板化，并允许直接切换到模板 API，不保留旧的显式函数包装。

## 目标

将 MiniRpc codec 改为“按类型分发”的模板接口：

- `EncodeMessage<T>()`
- `DecodeMessage<T>()`
- `CodecTraits<T>`

同时保持以下内容不变：

- 二进制协议格式
- request/reply 结构体定义
- client 对外行为
- 错误返回语义

## 核心结论

本次重构采用“模板外壳 + 显式特化”的方式，不采用自动反射或字段级元编程。

也就是说：

- 对外统一使用模板 API
- 每个消息类型通过 `CodecTraits<T>` 显式特化提供 `Encode/Decode`
- 字段读写仍然显式调用 `ByteWriter` / `ByteReader`

这样可以同时满足：

- API 统一
- 扩展点稳定
- 协议格式可读
- 模板复杂度可控

## 方案对比

### 方案 1：保留旧函数，内部转发到模板

优点：

- 对现有调用兼容最好

缺点：

- 会长期保留两套 API
- 后续调用风格容易混用

### 方案 2：直接切到纯模板 API

优点：

- 接口最统一
- 新类型扩展路径最明确
- 调用点命名收敛

缺点：

- 需要一次性修改现有调用点

### 方案 3：进一步做自动字段模板序列化

优点：

- 表面样板更少

缺点：

- 过度设计
- 协议顺序更隐蔽
- 可维护性下降

最终选择方案 2。

## 对外 API 设计

`include/apps/minirpc/common/minirpc_codec.h` 将提供：

```cpp
template <typename T>
struct CodecTraits;

template <typename T>
bool EncodeMessage(const T& value, std::vector<uint8_t>* bytes);

template <typename T>
bool DecodeMessage(const std::vector<uint8_t>& bytes, T* value);
```

每个消息类型通过显式特化实现：

```cpp
template <>
struct CodecTraits<AddRequest> {
  static bool Encode(const AddRequest& value, std::vector<uint8_t>* bytes);
  static bool Decode(const std::vector<uint8_t>& bytes, AddRequest* value);
};
```

旧的 `EncodeXxx()` / `DecodeXxx()` 接口全部删除。

## 实现组织

模板主入口和 `CodecTraits<T>` 特化定义放在头文件中。

原因：

- 模板和显式特化需要在使用点可见
- 这样 `MiniRpcAsyncClient` 和 `MiniRpcClient` 在各自翻译单元中可以直接实例化调用

字段读写逻辑继续保持显式：

- `EchoRequest/EchoReply` 使用 `WriteString/ReadString`
- `AddRequest/AddReply` 使用 `WriteInt32/ReadInt32`
- `SleepRequest` 使用 `WriteUint32/ReadUint32`
- `SleepReply` 使用 `WriteInt32/ReadInt32`

辅助函数如 `AssignBytes()` 也移动到头文件中，作为 `inline` 内部 helper 使用。

`src/apps/minirpc/common/minirpc_codec.cpp` 不再承载编解码实现，可保留为空翻译单元或删除其功能性内容。

## 调用点调整

`MiniRpcAsyncClient` 统一改为：

- `EncodeMessage<EchoRequest>(...)`
- `EncodeMessage<AddRequest>(...)`
- `EncodeMessage<SleepRequest>(...)`

`MiniRpcClient` 统一改为：

- `DecodeMessage<EchoReply>(...)`
- `DecodeMessage<AddReply>(...)`
- `DecodeMessage<SleepReply>(...)`

这样 client 层只依赖“消息类型”，不依赖一组展开的 codec 函数名。

## 错误处理

错误语义保持不变：

- `bytes == nullptr` 或输出对象为空时返回 `false`
- 编解码失败时调用方继续返回已有错误码
- 同步 client 的 `ProtocolMismatch` 语义保持不变

模板化不引入新的异常或错误码。

## 测试策略

测试分三层：

1. `minirpc_codec_test`
   - 直接改成验证 `EncodeMessage<T>()` / `DecodeMessage<T>()`
   - 确认现有消息类型编码和解码结果不变

2. `minirpc_client_test`
   - 作为调用链回归测试
   - 确认模板 codec 接入 client 后行为不变

3. 头文件可见性
   - 继续通过现有 header/build 测试保证模板定义放头文件后能正常编译

## 非目标

本次不做以下内容：

- 不新增消息类型
- 不修改协议字段布局
- 不引入自动反射或宏注册
- 不推广到整个 MemRpc 框架通用 codec
- 不修改服务端 handler 逻辑

## 预期收益

- 新增消息类型时只需要添加 `CodecTraits<T>` 特化
- client 调用点更统一
- 编解码 API 更容易复用到后续其他模板调用路径
- 保持当前协议实现的直观性，不牺牲可读性换取过度抽象
