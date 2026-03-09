# MiniRpc 优先的框架通用化设计

## 背景

当前仓库已经同时存在三条线：

- 共享内存 RPC 框架 `memrpc`
- 为兼容旧接口而逐步演化出来的 VPS 应用层
- 若干过渡目录与过渡命名空间

现阶段最核心的问题不是继续扩 VPS 业务能力，而是先把框架边界收干净。否则后面无论接 Harmony SA、迁移 `VirusEngineManager`，还是新增其他 IPC 应用，都会反复返工。

用户进一步明确了新的约束：

1. 框架和应用层必须彻底解耦
2. 所有命名空间统一挂在 `OHOS::Security::VirusProtectionService` 顶层下
3. 框架层使用独立子命名空间，避免业务语义污染
4. 先做一个最小 RPC demo app，验证跨进程能力和框架泛用性
5. VPS 应用层后续再按同样模式重构
6. 当前不引入宏生成或代码生成系统
7. 手写 codec 和 glue，但允许提取少量公共 helper 降低重复
8. 当前只维护 CMake，GN 等结构稳定后再统一翻译
9. 当前只保留请求与响应 `eventfd`，不引入独立事件通道

## 总体目标

后续仓库应收敛成：

- 纯框架层 `MemRpc`
- 最小验证应用 `MiniRpc`
- 复杂真实应用 `Vps`

其中：

- `MiniRpc` 用于验证框架本身是否真正通用
- `Vps` 用于验证复杂业务迁移是否可行
- 两个应用层彼此独立，只共同依赖框架层
- 监听型需求后续通过普通 `Poll...()` RPC 实现，不走子进程主动推送

## 命名空间设计

统一顶层命名空间：

```cpp
namespace OHOS::Security::VirusProtectionService {
}
```

在此基础上进一步划分：

- 框架层：
  - `OHOS::Security::VirusProtectionService::MemRpc`
- 最小 demo 应用：
  - `OHOS::Security::VirusProtectionService::MiniRpc`
- VPS 应用层：
  - `OHOS::Security::VirusProtectionService`
  - 或在需要时继续拆出更细子命名空间，但不脱离该顶层域

这样做的原因是：

- 满足统一域要求
- 框架仍有独立边界
- 应用层不会和框架符号直接混在一起

## 目录结构

后续建议以如下目录为目标：

```text
include/
  memrpc/
    core/
    client/
    server/
    compat/
  apps/
    minirpc/
      common/
      parent/
      child/
    vps/
      common/
      parent/
      child/

src/
  memrpc/
    core/
    client/
    server/
    compat/
  apps/
    minirpc/
      common/
      parent/
      child/
    vps/
      common/
      parent/
      child/
```

目录职责如下：

- `memrpc`
  - 共享内存布局
  - ring / slot / session
  - bootstrap / recovery
  - 公共异步客户端与服务端
- `apps/minirpc`
  - 用于验证框架能力的最小应用
- `apps/vps`
  - 真实复杂应用的兼容与迁移落点

临时目录如 `vps_demo`、框架侧业务 codec 目录、`EngineClient/EngineServer` 这类业务兼容层，不应作为最终结构长期保留。

## 框架分层

### 1. 公共异步层

这是框架的一等公民，负责：

- `RpcCall`
- `RpcReply`
- `RpcFuture`
- `RpcClient`
- `RpcServer`
- 会话恢复
- 超时控制

该层不出现任何业务语义：

- 不出现 `ScanTask`
- 不出现 `ScanResult`
- 不出现 `VirusEngineManager`

框架不再内建任何业务兼容层。

旧接口兼容由各应用自己完成，例如：

- `MiniRpcClient`
- 未来的 `VirusEngineManager`

框架只负责 transport，不负责替应用定义“同步旧接口应该长什么样”。

## MiniRpc 应用设计

`MiniRpc` 是一个完全独立的最小应用层，用于验证框架是否真的泛用，而不是只为 VPS 定制。

### 目标能力

第一版只保留 3 项能力：

1. `Echo`
   - 请求：字符串
   - 响应：字符串
2. `Add`
   - 请求：两个整数
   - 响应：求和结果
3. `Sleep`
   - 请求：延迟毫秒数
   - 响应：状态码

这 4 项足以覆盖：

- 基本 request/response
- 定长和变长 payload
- 同步包装
- 异步调用
- 并发与优先级
- 超时
- 子进程恢复后的继续使用

### 内外接口分层

`MiniRpc` 也采用两层结构：

- 内部异步接口：
  - `MiniRpcAsyncClient`
- 对外同步包装：
  - `MiniRpcClient`

父进程内部全部基于异步事务模型实现，对外提供同步兼容外观。

这与后续 VPS 的迁移方向保持一致。

### 父进程职责

`apps/minirpc/parent` 负责：

- `MiniRpcAsyncClient`
- `MiniRpcClient`
- 启动或连接子进程
- 同步等待包装

### 子进程职责

`apps/minirpc/child` 负责：

- `MiniRpcService`
- 注册 handler
- 执行 `Echo/Add/Sleep`

## VPS 应用定位

VPS 不再作为框架验证第一优先级，而是后续迁移目标。

当前阶段对 VPS 的策略是：

- 只保留设计与目录落点
- 当前不纳入主构建
- 待 `MiniRpc` 把框架能力验证透后，再回到 VPS
- VPS 重构方式完全参考 `MiniRpc`：
  - `common`
  - `parent`
  - `child`
  - 内部异步
  - 对外同步兼容

## 编解码策略

当前不引入 `X-macro`、Python 代码生成或其他自动生成系统。

理由：

1. 当前业务结构不简单，生成系统容易演化成新的复杂度来源
2. 现阶段的核心问题是框架边界，而不是 codec 样板数量
3. 用户明确要求保持简单、低复杂度、易维护

因此当前策略为：

- request/response codec 手写
- RPC glue 手写
- 仅提取少量公共 helper 降低重复

可接受的公共 helper 包括：

- 写请求头 + payload 的小函数
- 读取响应并做状态映射的小函数
- server 侧读取请求和写回响应的小函数
- 轮询型 RPC 的重复调度小函数

不做：

- 宏序列化系统
- 反射式注册系统
- 外部 schema 生成流程

## 监听与异常处理

框架级故障处理继续遵守之前已确认的边界：

- 子进程死亡通过 bootstrap 回调通知
- 当前 session 立即失效
- 旧 session 上 pending 请求立即收口
- 下一次调用懒恢复：
  - `StartEngine()`
  - `Connect()`
  - 切到新 session

对于未来的监听型能力，当前建议采用：

- 主进程保留真实 listener
- 子进程只维护事件数据队列
- 主进程后台线程定期发 `Poll...()` RPC
- 事件通过普通响应队列返回
- 主进程在本地广播给 listener

如果后面某个场景确实存在强实时主动推送需求，再评估是否补独立事件通道；当前不作为主线。

## 当前阶段的实施顺序

建议按以下顺序推进：

1. 统一命名空间到 `OHOS::Security::VirusProtectionService`
2. 把框架公共代码和兼容代码继续收拢到 `memrpc`
3. 新建并实现 `apps/minirpc`
4. 用 `MiniRpc` 补齐和验证跨进程能力
5. 清理过渡目录与过渡 codec
6. 最后再回到 `apps/vps`

## 非目标

本阶段明确不做：

- VPS 复杂业务进一步扩展
- 真实 Harmony SA 细节接线
- GN 同步维护
- 宏或 Python 代码生成
- 流式 RPC
- 取消执行
- 复杂版本协商

## 结论

这次重构的核心不是继续给 VPS 补功能，而是先把框架做成真正可复用的公共层。

第一阶段最重要的成功标准是：

- `MemRpc` 框架不依赖业务语义
- `MiniRpc` 能完整验证最小跨进程 RPC 能力
- `Vps` 保持为后续迁移目标，而不再反向牵引框架结构
