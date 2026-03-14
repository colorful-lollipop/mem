#ifndef MEMRPC_CORE_PROTOCOL_H_
#define MEMRPC_CORE_PROTOCOL_H_

#include <array>
#include <cstdint>

namespace MemRpc {

// 共享内存头和 ring 布局版本号；双方必须严格一致。
inline constexpr uint32_t SHARED_MEMORY_MAGIC = 0x4d454d52U;
inline constexpr uint32_t PROTOCOL_VERSION = 3U;
inline constexpr uint32_t DEFAULT_MAX_REQUEST_BYTES = 4U * 1024U;
inline constexpr uint32_t DEFAULT_MAX_RESPONSE_BYTES = 4U * 1024U;

// Framework-level opcode type. Applications define their own typed enums and
// cast to Opcode when building RpcCall / registering handlers.
using Opcode = uint16_t;
inline constexpr Opcode OPCODE_INVALID = 0;

enum class ResponseMessageKind : uint16_t {  // NOLINT(performance-enum-size)
  Reply = 0,
  Event = 1,
};

enum class SlotRuntimeStateCode : uint32_t {  // NOLINT(performance-enum-size)
  Free = 0,
  Admitted = 1,
  Queued = 2,
  Executing = 3,
  Responding = 4,
  Ready = 5,
  Consumed = 6,
};

struct RequestRingEntry {
  // Ring entry 只保存路由/定位信息，真正的请求体放在 slot payload 中。
  uint64_t requestId = 0;
  uint32_t slotIndex = 0;
  uint16_t opcode = OPCODE_INVALID;
  uint16_t flags = 0;
  uint32_t enqueueMonoMs = 0;
  uint32_t payloadSize = 0;
  uint32_t reserved = 0;
};

struct ResponseRingEntry {
  uint64_t requestId = 0;
  uint32_t slotIndex = 0;
  ResponseMessageKind messageKind = ResponseMessageKind::Reply;
  uint16_t reserved = 0;
  uint32_t statusCode = 0;
  int32_t errorCode = 0;
  uint32_t eventDomain = 0;
  uint32_t eventType = 0;
  uint32_t flags = 0;
  uint32_t resultSize = 0;
};

static_assert(sizeof(RequestRingEntry) == 32U, "RequestRingEntry size must stay fixed");

struct SlotRuntimeState {
  uint64_t requestId = 0;
  SlotRuntimeStateCode state = SlotRuntimeStateCode::Free;
  uint32_t workerId = 0;
  uint32_t enqueueMonoMs = 0;
  uint32_t startExecMonoMs = 0;
  uint32_t lastHeartbeatMonoMs = 0;
  uint32_t seq = 0;  // Seqlock version for consistent shared-memory snapshots.
};

static_assert(sizeof(SlotRuntimeState) == 32U, "SlotRuntimeState size must stay fixed");

struct RpcRequestHeader {
  // 这是 slot 内部请求头，和 RequestRingEntry 一起构成一次完整调用。
  uint32_t queueTimeoutMs = 0;
  uint32_t execTimeoutMs = 0;
  uint32_t flags = 0;
  uint32_t priority = 0;
  uint16_t opcode = 0;
  uint16_t reserved = 0;
  uint32_t payloadSize = 0;
};

struct SlotPayload {
  SlotRuntimeState runtime{};
  RpcRequestHeader request{};
};

struct ResponseSlotHeader {
  uint32_t requestSlotIndex = 0;
  uint32_t payloadSize = 0;
};

struct ResponseSlotRuntimeState {
  uint64_t requestId = 0;
  SlotRuntimeStateCode state = SlotRuntimeStateCode::Free;
  uint32_t requestSlotIndex = 0;
  uint32_t publishMonoMs = 0;
  uint32_t lastUpdateMonoMs = 0;
  std::array<uint32_t, 2> reserved{};
};

struct ResponseSlotPayload {
  ResponseSlotRuntimeState runtime{};
  ResponseSlotHeader response{};
};

constexpr uint32_t ComputeSlotSize(uint32_t max_request_bytes,
                                   uint32_t max_response_bytes) {
  static_cast<void>(max_response_bytes);
  // 当前响应体直接写入 response ring，不占 slot 空间。
  return sizeof(SlotPayload) + max_request_bytes;
}

constexpr uint32_t ComputeResponseSlotSize(uint32_t max_response_bytes) {
  return sizeof(ResponseSlotPayload) + max_response_bytes;
}

}  // namespace MemRpc

#endif  // MEMRPC_CORE_PROTOCOL_H_
