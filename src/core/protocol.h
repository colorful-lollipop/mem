#ifndef MEMRPC_CORE_PROTOCOL_H_
#define MEMRPC_CORE_PROTOCOL_H_

#include <cstdint>

namespace memrpc {

// 共享内存头和 ring 布局版本号；双方必须严格一致。
inline constexpr uint32_t kSharedMemoryMagic = 0x4d454d52u;
inline constexpr uint32_t kProtocolVersion = 3u;
inline constexpr uint32_t kDefaultMaxRequestBytes = 4u * 1024u;
inline constexpr uint32_t kDefaultMaxResponseBytes = 4u * 1024u;

// Framework-level opcode type. Applications define their own typed enums and
// cast to Opcode when building RpcCall / registering handlers.
using Opcode = uint16_t;
inline constexpr Opcode OPCODE_INVALID = 0;

enum class ResponseMessageKind : uint16_t {
  Reply = 0,
  Event = 1,
};

enum class SlotRuntimeStateCode : uint32_t {
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
  uint64_t request_id = 0;
  uint32_t slot_index = 0;
  uint16_t opcode = OPCODE_INVALID;
  uint16_t flags = 0;
  uint32_t enqueue_mono_ms = 0;
  uint32_t payload_size = 0;
  uint32_t reserved = 0;
};

struct ResponseRingEntry {
  uint64_t request_id = 0;
  uint32_t slot_index = 0;
  ResponseMessageKind message_kind = ResponseMessageKind::Reply;
  uint16_t reserved = 0;
  uint32_t status_code = 0;
  int32_t engine_errno = 0;
  int32_t detail_code = 0;
  uint32_t event_domain = 0;
  uint32_t event_type = 0;
  uint32_t flags = 0;
  uint32_t result_size = 0;
};

static_assert(sizeof(RequestRingEntry) == 32u, "RequestRingEntry size must stay fixed");

struct SlotRuntimeState {
  uint64_t request_id = 0;
  SlotRuntimeStateCode state = SlotRuntimeStateCode::Free;
  uint32_t worker_id = 0;
  uint32_t enqueue_mono_ms = 0;
  uint32_t start_exec_mono_ms = 0;
  uint32_t last_heartbeat_mono_ms = 0;
  uint32_t reserved = 0;
};

static_assert(sizeof(SlotRuntimeState) == 32u, "SlotRuntimeState size must stay fixed");

struct RpcRequestHeader {
  // 这是 slot 内部请求头，和 RequestRingEntry 一起构成一次完整调用。
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint32_t flags = 0;
  uint32_t priority = 0;
  uint16_t opcode = 0;
  uint16_t reserved = 0;
  uint32_t payload_size = 0;
};

struct SlotPayload {
  SlotRuntimeState runtime{};
  RpcRequestHeader request{};
};

struct ResponseSlotHeader {
  uint32_t request_slot_index = 0;
  uint32_t payload_size = 0;
};

struct ResponseSlotRuntimeState {
  uint64_t request_id = 0;
  SlotRuntimeStateCode state = SlotRuntimeStateCode::Free;
  uint32_t request_slot_index = 0;
  uint32_t publish_mono_ms = 0;
  uint32_t last_update_mono_ms = 0;
  uint32_t reserved[2]{};
};

struct ResponseSlotPayload {
  ResponseSlotRuntimeState runtime{};
  ResponseSlotHeader response{};
};

inline constexpr uint32_t ComputeSlotSize(uint32_t max_request_bytes,
                                          uint32_t max_response_bytes) {
  static_cast<void>(max_response_bytes);
  // 当前响应体直接写入 response ring，不占 slot 空间。
  return sizeof(SlotPayload) + max_request_bytes;
}

inline constexpr uint32_t ComputeResponseSlotSize(uint32_t max_response_bytes) {
  return sizeof(ResponseSlotPayload) + max_response_bytes;
}

}  // namespace memrpc

#endif  // MEMRPC_CORE_PROTOCOL_H_
