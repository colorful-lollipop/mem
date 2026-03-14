# MemRpc 当前方案

## 定位

- `memrpc` 只覆盖小包、高频、同步业务调用的主路径。
- 共享内存协议固定为 512B ring entry；不再有 request/response slot、slot pool 或 slot runtime state。
- `virus_executor_service` 对外只暴露同步 typed API；框架内部仍可保留异步 worker、response writer 和 watchdog。
- 超过 inline 上限的请求不再尝试塞进共享内存，而是直接走同步 `AnyCall` 控制面兜底。
- 如果请求已经走了 `memrpc`，但执行后发现响应或事件超限，则直接返回 `PayloadTooLarge`；不做 heartbeat 拉取、不自动 fallback、不自动重试。

## 已落地设计

- `RequestRingEntry` 固定 512B，请求 inline 上限为 `480B`。
- `ResponseRingEntry` 固定 512B，响应和事件 inline 上限为 `472B`。
- 会话继续保留三条 ring：
  - high request ring
  - normal request ring
  - response ring
- `eventfd` 继续分成消息唤醒和 credit 唤醒两类；背压只和 ring 空间有关。
- `RpcClient` 直接把请求写入 ring entry，pending 以 `requestId` 跟踪。
- `RpcServer` 直接从 ring entry 构造调用，小响应直接写回 response ring。
- `PublishEvent()` 只支持小包；超限立即失败。
- `IVesControl::AnyCall()`、`VesControlProxy::AnyCall()`、`VirusExecutorService::AnyCall()` 已落地。
- `VesClient` 在编码请求后按大小分流：
  - `payload <= DEFAULT_MAX_REQUEST_BYTES`：走 `memrpc`
  - `payload > DEFAULT_MAX_REQUEST_BYTES`：走同步 `AnyCall`

## 设计边界

- `memrpc` 目标是覆盖 90% 的正常小包场景，不追求统一承载所有 payload。
- 大请求可以走 `AnyCall` 或其他 IPC。
- 大响应、大事件当前就是失败语义，不做补救链路。
- 不再把历史 slot 设计当成后续优化方向。

## 当前规范入口

- 当前方案总览：[docs/guide.md](/root/mem/docs/guide.md)
- 通信与并发边界：[docs/architecture.md](/root/mem/docs/architecture.md)
- 业务接入方式：[docs/porting_guide.md](/root/mem/docs/porting_guide.md)
- demo 与验证入口：[docs/demo_guide.md](/root/mem/docs/demo_guide.md)
- 历史计划归档说明：[docs/plans/guide.md](/root/mem/docs/plans/guide.md)
