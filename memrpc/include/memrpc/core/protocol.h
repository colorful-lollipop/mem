#ifndef MEMRPC_CORE_PROTOCOL_H_
#define MEMRPC_CORE_PROTOCOL_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace MemRpc {

// 共享内存头和 ring 布局版本号；双方必须严格一致。
inline constexpr uint32_t SHARED_MEMORY_MAGIC = 0x4d454d52U;
inline constexpr uint32_t PROTOCOL_VERSION = 6U;
inline constexpr uint32_t RING_ENTRY_BYTES = 8192U;

// Framework-level opcode type. Applications define their own typed enums and
// cast to Opcode when building RpcCall / registering handlers.
using Opcode = uint16_t;
inline constexpr Opcode OPCODE_INVALID = 0;

enum class ResponseMessageKind : uint16_t {  // NOLINT(performance-enum-size)
    Reply = 0,
    Event = 1,
};

template <typename... Ts>
constexpr std::size_t SumFieldBytes()
{
    return (0U + ... + sizeof(Ts));
}

struct RequestRingEntry {
    uint64_t requestId = 0;
    uint32_t execTimeoutMs = 0;
    uint16_t opcode = OPCODE_INVALID;
    uint8_t priority = 0;
    uint8_t reserved0 = 0;
    uint32_t payloadSize = 0;
    static constexpr std::size_t HEADER_BYTES = SumFieldBytes<uint64_t, uint32_t, Opcode, uint8_t, uint8_t, uint32_t>();
    static constexpr std::size_t INLINE_PAYLOAD_BYTES = RING_ENTRY_BYTES - HEADER_BYTES;
    std::array<uint8_t, INLINE_PAYLOAD_BYTES> payload{};
};

struct ResponseRingEntry {
    uint64_t requestId = 0;
    uint32_t statusCode = 0;
    int32_t errorCode = 0;
    uint32_t eventDomain = 0;
    uint32_t eventType = 0;
    uint32_t flags = 0;
    uint32_t resultSize = 0;
    ResponseMessageKind messageKind = ResponseMessageKind::Reply;
    uint16_t reserved = 0;
    uint32_t reserved0 = 0;
    static constexpr std::size_t HEADER_BYTES = SumFieldBytes<uint64_t,
                                                              uint32_t,
                                                              int32_t,
                                                              uint32_t,
                                                              uint32_t,
                                                              uint32_t,
                                                              uint32_t,
                                                              ResponseMessageKind,
                                                              uint16_t,
                                                              uint32_t>();
    static constexpr std::size_t INLINE_PAYLOAD_BYTES = RING_ENTRY_BYTES - HEADER_BYTES;
    std::array<uint8_t, INLINE_PAYLOAD_BYTES> payload{};
};

inline constexpr uint32_t DEFAULT_MAX_REQUEST_BYTES = static_cast<uint32_t>(RequestRingEntry::INLINE_PAYLOAD_BYTES);
inline constexpr uint32_t DEFAULT_MAX_RESPONSE_BYTES = static_cast<uint32_t>(ResponseRingEntry::INLINE_PAYLOAD_BYTES);

struct SharedMemoryLayoutDefaults {
    uint32_t highRingSize;
    uint32_t normalRingSize;
    uint32_t responseRingSize;
    uint32_t maxRequestBytes;
    uint32_t maxResponseBytes;
};

// Single source of truth for default shared-memory sizing.
inline constexpr SharedMemoryLayoutDefaults DEFAULT_SHARED_MEMORY_LAYOUT{
    8U,
    8U,
    8U,
    DEFAULT_MAX_REQUEST_BYTES,
    DEFAULT_MAX_RESPONSE_BYTES,
};

static_assert(offsetof(RequestRingEntry, payload) == RequestRingEntry::HEADER_BYTES,
              "RequestRingEntry header size constant must match payload offset");
static_assert(offsetof(ResponseRingEntry, payload) == ResponseRingEntry::HEADER_BYTES,
              "ResponseRingEntry header size constant must match payload offset");
static_assert(sizeof(RequestRingEntry) == RING_ENTRY_BYTES, "RequestRingEntry size must stay fixed");
static_assert(sizeof(ResponseRingEntry) == RING_ENTRY_BYTES, "ResponseRingEntry size must stay fixed");

}  // namespace MemRpc

#endif  // MEMRPC_CORE_PROTOCOL_H_
