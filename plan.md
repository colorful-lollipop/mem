 # MemRpc 固定 Entry 化与同步 AnyCall 兜底方案

  ## Summary

  目标调整为：memrpc 只覆盖固定小包同步 RPC 的主路径，内部仍可保留异步 worker/response writer 实现；对外接口只提供同步语义。共享内存协议从“ring + slot”收缩为“固定大小 request/response ring entry”，删除 slot 及其状态机、池、回收逻辑。
  超过固定 entry 容量的请求，客户端在发起前直接走同步 AnyCall 控制面兜底；如果请求已走 memrpc，但服务端执行后发现响应过大，则直接失败，不做二次拉取、不走 heartbeat、不自动重试到 AnyCall。

  ## Key Changes

  - 共享内存协议
      - 保留 high request ring、normal request ring、response ring。
      - 删除 request slot、response slot、slot pool、response slot pool、slot runtime state。
      - RequestRingEntry 改为固定大小内嵌请求体，字段包含：requestId、opcode、flags、priority、queueTimeoutMs、execTimeoutMs、payloadSize、payload[INLINE_BYTES]。
      - ResponseRingEntry 改为固定大小内嵌响应体，字段包含：requestId、messageKind、statusCode、errorCode、事件字段、resultSize、payload[INLINE_BYTES]。
      - entry 总大小按 512B 设计，请求和响应使用同一固定容量模型。
      - bump PROTOCOL_VERSION，并重算 shm layout；layout 中不再有 slot 区域。
  - memrpc 运行时
      - RpcClient 提交请求时直接写 ring entry，不再 reserve/release slot。
      - pending 跟踪改为 requestId -> future/info，响应按 requestId 直接完成。
      - admission/backpressure 仅由 request ring 是否满决定；response backpressure 仅由 response ring 是否满决定；保留 eventfd credit。
      - RpcServer 从 ring entry 直接构造 RpcServerCall，handler 执行后把小响应直接写回 response ring。
      - 如果 handler 产出的 reply payload 超过 inline 容量，server 返回明确错误码，结束该请求；不转 heartbeat，不转异步拉取。
      - PublishEvent 同样仅支持小包；超限直接失败。
  - 同步 AnyCall 兜底
      - 在 IVesControl / VesControlProxy / VirusExecutorService 新增同步 AnyCall(const VesAnyCallRequest&, VesAnyCallReply&)。
      - AnyCall 只用于超大请求或显式走控制面的同步调用，不提供异步对外接口。
      - VesClient 在编码请求后先判断大小：
          - payload <= MEMRPC_INLINE_LIMIT：走 memrpc 同步调用。
          - payload > MEMRPC_INLINE_LIMIT：直接走同步 AnyCall。
      - AnyCall 复用现有 opcode/handler 语义，不新增独立业务协议层。
      - 不实现“memrpc 执行后发现响应大，再自动 fallback 到 AnyCall”的补救路径；避免重复执行和副作用不确定性。

  ## Public APIs / Interfaces / Types

  - memrpc/core/protocol.h
      - 重定义 RequestRingEntry / ResponseRingEntry 为固定大小内嵌 payload 格式。
      - 删除 SlotPayload、ResponseSlotPayload、slot runtime 相关类型。
      - 新增固定容量常量，如 MEMRPC_INLINE_PAYLOAD_LIMIT。
  - memrpc/core/session.h
      - 删除 slot 访问相关 API，仅保留 ring push/pop。
  - virus_executor_service/include/transport/ves_control_interface.h
      - 新增 VesAnyCallRequest、VesAnyCallReply。
      - IVesControl 新增 AnyCall(...)。
  - virus_executor_service/include/transport/ves_control_proxy.h
      - 新增同步 AnyCall(...)。
  - virus_executor_service/include/service/virus_executor_service.h
      - 新增 AnyCall(...) override。
  - virus_executor_service/include/client/ves_client.h
      - 对外仍保留同步 typed API，不新增异步公开接口。
  ## Test Plan
  - 协议/layout
      - 验证新 ring entry 固定大小、PROTOCOL_VERSION 变化、layout 不含 slot 区域。
      - attach/remap 使用新 layout 正常，旧版本 shm 被拒绝。
      - high/normal 双队列语义保持。
      - request ring 满时 admission wait/timeout 正常。
      - response ring 满时 response credit 生效。
      - server crash / session reset 后 pending 同步调用失败并可恢复，不依赖 slot 状态。
  - 超限行为
      - 请求超限时，VesClient 直接改走 AnyCall。
      - 响应超限时，memrpc 返回明确错误，不自动重试、不重复执行 handler。
      - event 超限时返回失败。
  - AnyCall
      - 同一业务 opcode 在 memrpc 路径和 AnyCall 路径结果一致。
      - AnyCall 正确透传 opcode、payload、timeout、status、errorCode。
  ## Assumptions And Defaults

  - 固定 entry 目标大小为 512B，可用 payload 上限由头部字段扣减后确定。
  - 保留双请求队列，不合并 high/normal。