#ifndef MEMRPC_CORE_PROTOCOL_H_
#define MEMRPC_CORE_PROTOCOL_H_

#include <cstdint>

namespace memrpc {

inline constexpr uint32_t kSharedMemoryMagic = 0x4d454d52u;
inline constexpr uint32_t kProtocolVersion = 1u;
inline constexpr uint32_t kMaxFilePathSize = 1024u;
inline constexpr uint32_t kMaxMessageSize = 512u;

enum class Opcode : uint16_t {
  ScanFile = 1,
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
  uint32_t status_code = 0;
  int32_t engine_errno = 0;
  uint32_t result_size = 0;
};

static_assert(sizeof(RequestRingEntry) == 32u, "RequestRingEntry size must stay fixed");
static_assert(sizeof(ResponseRingEntry) == 24u, "ResponseRingEntry size must stay fixed");

struct SlotPayload {
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint32_t flags = 0;
  uint32_t priority = 0;
  uint32_t file_path_length = 0;
  char file_path[kMaxFilePathSize]{};
  uint32_t status_code = 0;
  uint32_t verdict = 0;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  uint32_t message_length = 0;
  char message[kMaxMessageSize]{};
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_PROTOCOL_H_
