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
  uint32_t high_ring_size = 0;
  uint32_t normal_ring_size = 0;
  uint32_t response_ring_size = 0;
  uint32_t slot_count = 0;
  uint32_t slot_size = ComputeSlotSize(kDefaultMaxRequestBytes, kDefaultMaxResponseBytes);
  uint32_t max_request_bytes = kDefaultMaxRequestBytes;
  uint32_t max_response_bytes = kDefaultMaxResponseBytes;
  uint32_t high_reserved_request_slots = 0;
};

struct Layout {
  std::size_t high_ring_offset = 0;
  std::size_t normal_ring_offset = 0;
  std::size_t response_ring_offset = 0;
  std::size_t slot_pool_offset = 0;
  std::size_t response_slot_pool_offset = 0;
  std::size_t response_slots_offset = 0;
  std::size_t total_size = 0;
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
  uint32_t magic = kSharedMemoryMagic;
  uint32_t protocol_version = kProtocolVersion;
  uint64_t session_id = 0;
  uint32_t session_state = 0;
  uint32_t client_attached = 0;
  uint32_t active_client_pid = 0;
  uint32_t high_ring_size = 0;
  uint32_t normal_ring_size = 0;
  uint32_t response_ring_size = 0;
  uint32_t slot_count = 0;
  uint32_t high_reserved_request_slots = 0;
  uint32_t slot_size = ComputeSlotSize(kDefaultMaxRequestBytes, kDefaultMaxResponseBytes);
  uint32_t max_request_bytes = kDefaultMaxRequestBytes;
  uint32_t max_response_bytes = kDefaultMaxResponseBytes;
  RingCursor high_ring{};
  RingCursor normal_ring{};
  RingCursor response_ring{};
  pthread_mutex_t client_state_mutex{};
};

inline Layout ComputeLayout(const LayoutConfig& config) {
  Layout layout;
  layout.high_ring_offset = sizeof(SharedMemoryHeader);
  layout.normal_ring_offset =
      layout.high_ring_offset + sizeof(RequestRingEntry) * config.high_ring_size;
  layout.response_ring_offset =
      layout.normal_ring_offset + sizeof(RequestRingEntry) * config.normal_ring_size;
  layout.slot_pool_offset =
      layout.response_ring_offset + sizeof(ResponseRingEntry) * config.response_ring_size;
  layout.response_slot_pool_offset = layout.slot_pool_offset +
                      static_cast<std::size_t>(config.slot_count) * config.slot_size;
  layout.response_slots_offset =
      layout.response_slot_pool_offset + ComputeSharedSlotPoolBytes(config.response_ring_size);
  layout.total_size = layout.response_slots_offset +
                      static_cast<std::size_t>(config.response_ring_size) *
                          ComputeResponseSlotSize(config.max_response_bytes);
  return layout;
}

}  // namespace memrpc

#endif  // MEMRPC_CORE_SHM_LAYOUT_H_
