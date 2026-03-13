#ifndef MEMRPC_CORE_SHM_LAYOUT_H_
#define MEMRPC_CORE_SHM_LAYOUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "core/protocol.h"
#include "core/slot_pool.h"

namespace memrpc {

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

struct SharedMemoryHeader {
  uint32_t magic = SHARED_MEMORY_MAGIC;
  uint32_t protocol_version = PROTOCOL_VERSION;
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
  layout.highRingOffset = sizeof(SharedMemoryHeader);
  layout.normalRingOffset =
      layout.highRingOffset + sizeof(RequestRingEntry) * config.highRingSize;
  layout.responseRingOffset =
      layout.normalRingOffset + sizeof(RequestRingEntry) * config.normalRingSize;
  layout.slotPoolOffset =
      layout.responseRingOffset + sizeof(ResponseRingEntry) * config.responseRingSize;
  layout.responseSlotPoolOffset = layout.slotPoolOffset +
                      static_cast<std::size_t>(config.slotCount) * config.slotSize;
  layout.responseSlotsOffset =
      layout.responseSlotPoolOffset + ComputeSharedSlotPoolBytes(config.responseRingSize);
  layout.totalSize = layout.responseSlotsOffset +
                      static_cast<std::size_t>(config.responseRingSize) *
                          ComputeResponseSlotSize(config.maxResponseBytes);
  return layout;
}

}  // namespace memrpc

#endif  // MEMRPC_CORE_SHM_LAYOUT_H_
