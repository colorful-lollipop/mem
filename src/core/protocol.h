#ifndef MEMRPC_CORE_PROTOCOL_H_
#define MEMRPC_CORE_PROTOCOL_H_

#include <cstdint>

namespace memrpc {

inline constexpr uint32_t kSharedMemoryMagic = 0x4d454d52u;
inline constexpr uint32_t kProtocolVersion = 1u;
inline constexpr uint32_t kDefaultMaxRequestBytes = 16u * 1024u;
inline constexpr uint32_t kDefaultMaxResponseBytes = 1024u;
inline constexpr uint32_t kMaxFilePathSize = 1024u;
inline constexpr uint32_t kMaxMessageSize = 512u;

enum class Opcode : uint16_t {
  ScanFile = 1,
  ScanBehavior = 2,
  VpsInit = 100,
  VpsDeInit = 101,
  VpsScanFile = 102,
  VpsScanBehavior = 103,
  VpsIsExistAnalysisEngine = 104,
  VpsCreateAnalysisEngine = 105,
  VpsDestroyAnalysisEngine = 106,
  VpsUpdateFeatureLib = 107,
  MiniEcho = 200,
  MiniAdd = 201,
  MiniSleep = 202,
};

enum class ResponseMessageKind : uint16_t {
  Reply = 0,
  Event = 1,
};

struct RequestRingEntry {
  uint64_t request_id = 0;
  uint32_t slot_index = 0;
  uint16_t opcode = static_cast<uint16_t>(Opcode::ScanFile);
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
  uint32_t event_domain = 0;
  uint32_t event_type = 0;
  uint32_t flags = 0;
  uint32_t result_size = 0;
  uint8_t event_payload[kDefaultMaxResponseBytes]{};
};

static_assert(sizeof(RequestRingEntry) == 32u, "RequestRingEntry size must stay fixed");

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
  RpcRequestHeader request{};
  RpcResponseHeader response{};
};

inline constexpr uint32_t ComputeSlotSize(uint32_t max_request_bytes,
                                          uint32_t max_response_bytes) {
  return sizeof(SlotPayload) + max_request_bytes + max_response_bytes;
}

struct ScanFileRequestPayload {
  uint32_t file_path_length = 0;
  char file_path[kMaxFilePathSize]{};
};

struct ScanFileResponsePayload {
  uint32_t verdict = 0;
  uint32_t message_length = 0;
  char message[kMaxMessageSize]{};
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_PROTOCOL_H_
