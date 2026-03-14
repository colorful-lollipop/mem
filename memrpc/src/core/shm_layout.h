#ifndef MEMRPC_CORE_SHM_LAYOUT_H_
#define MEMRPC_CORE_SHM_LAYOUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "memrpc/core/protocol.h"
#include "core/slot_pool.h"

namespace MemRpc {

struct LayoutConfig {
  uint32_t highRingSize = 32;
  uint32_t normalRingSize = 32;
  uint32_t responseRingSize = 64;
  uint32_t slotCount = 64;
  uint32_t slotSize = ComputeSlotSize(DEFAULT_MAX_REQUEST_BYTES, DEFAULT_MAX_RESPONSE_BYTES);
  uint32_t maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  uint32_t maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;
};

struct Layout {
  std::size_t highRingOffset = 0;
  std::size_t normalRingOffset = 0;
  std::size_t responseRingOffset = 0;
  std::size_t slotPoolOffset = 0;
  std::size_t responseSlotPoolOffset = 0;
  std::size_t responseSlotsOffset = 0;
  std::size_t totalSize = 0;
};

struct RingCursor {
  std::atomic<uint32_t> head{0};
  std::atomic<uint32_t> tail{0};
  uint32_t capacity = 0;
  uint32_t reserved = 0;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for SPSC ring correctness");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "std::atomic<uint32_t> must match uint32_t size for shared memory layout");

inline uint32_t RingCount(const RingCursor& cursor) {
  return cursor.tail.load(std::memory_order_relaxed) -
         cursor.head.load(std::memory_order_relaxed);
}

constexpr std::size_t AlignOffset(std::size_t offset, std::size_t alignment) {
  return alignment == 0 ? offset : ((offset + alignment - 1) / alignment) * alignment;
}

constexpr bool IsAlignedValue(uint32_t value, std::size_t alignment) {
  return alignment != 0 && value % alignment == 0;
}

constexpr bool HasAlignedPayloadSizes(uint32_t max_request_bytes, uint32_t max_response_bytes) {
  return IsAlignedValue(max_request_bytes, alignof(SlotPayload)) &&
         IsAlignedValue(max_response_bytes, alignof(ResponseSlotPayload));
}

struct SharedMemoryHeader {
  uint32_t magic = SHARED_MEMORY_MAGIC;
  uint32_t protocolVersion = PROTOCOL_VERSION;
  uint64_t sessionId = 0;
  uint32_t sessionState = 0;
  uint32_t clientAttached = 0;
  uint32_t activeClientPid = 0;
  uint32_t highRingSize = 32;
  uint32_t normalRingSize = 32;
  uint32_t responseRingSize = 64;
  uint32_t slotCount = 64;
  uint32_t slotSize = ComputeSlotSize(DEFAULT_MAX_REQUEST_BYTES, DEFAULT_MAX_RESPONSE_BYTES);
  uint32_t maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  uint32_t maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;
  RingCursor highRing{};
  RingCursor normalRing{};
  RingCursor responseRing{};
  pthread_mutex_t clientStateMutex{};
};

inline Layout ComputeLayout(const LayoutConfig& config) {
  Layout layout;
  layout.highRingOffset =
      AlignOffset(sizeof(SharedMemoryHeader), alignof(RequestRingEntry));
  layout.normalRingOffset = AlignOffset(
      layout.highRingOffset + sizeof(RequestRingEntry) * config.highRingSize,
      alignof(RequestRingEntry));
  layout.responseRingOffset = AlignOffset(
      layout.normalRingOffset + sizeof(RequestRingEntry) * config.normalRingSize,
      alignof(ResponseRingEntry));
  layout.slotPoolOffset = AlignOffset(
      layout.responseRingOffset + sizeof(ResponseRingEntry) * config.responseRingSize,
      alignof(SlotPayload));
  layout.responseSlotPoolOffset = AlignOffset(
      layout.slotPoolOffset + static_cast<std::size_t>(config.slotCount) * config.slotSize,
      alignof(SharedSlotPoolHeader));
  layout.responseSlotsOffset = AlignOffset(
      layout.responseSlotPoolOffset + ComputeSharedSlotPoolBytes(config.responseRingSize),
      alignof(ResponseSlotPayload));
  layout.totalSize = layout.responseSlotsOffset +
                      static_cast<std::size_t>(config.responseRingSize) *
                          ComputeResponseSlotSize(config.maxResponseBytes);
  return layout;
}

}  // namespace MemRpc

#endif  // MEMRPC_CORE_SHM_LAYOUT_H_
