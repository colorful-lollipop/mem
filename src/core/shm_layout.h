#ifndef MEMRPC_CORE_SHM_LAYOUT_H_
#define MEMRPC_CORE_SHM_LAYOUT_H_

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "core/protocol.h"

namespace memrpc {

struct LayoutConfig {
  uint32_t high_ring_size = 0;
  uint32_t normal_ring_size = 0;
  uint32_t response_ring_size = 0;
  uint32_t slot_count = 0;
  uint32_t slot_size = sizeof(SlotPayload);
};

struct Layout {
  std::size_t high_ring_offset = 0;
  std::size_t normal_ring_offset = 0;
  std::size_t response_ring_offset = 0;
  std::size_t slot_pool_offset = 0;
  std::size_t total_size = 0;
};

struct RingCursor {
  uint32_t head = 0;
  uint32_t tail = 0;
  uint32_t size = 0;
  uint32_t capacity = 0;
};

struct SharedMemoryHeader {
  uint32_t magic = kSharedMemoryMagic;
  uint32_t protocol_version = kProtocolVersion;
  uint64_t session_id = 0;
  uint32_t high_ring_size = 0;
  uint32_t normal_ring_size = 0;
  uint32_t response_ring_size = 0;
  uint32_t slot_count = 0;
  uint32_t slot_size = sizeof(SlotPayload);
  RingCursor high_ring{};
  RingCursor normal_ring{};
  RingCursor response_ring{};
  pthread_mutex_t high_ring_mutex{};
  pthread_mutex_t normal_ring_mutex{};
  pthread_mutex_t response_ring_mutex{};
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
  layout.total_size = layout.slot_pool_offset +
                      static_cast<std::size_t>(config.slot_count) * config.slot_size;
  return layout;
}

}  // namespace memrpc

#endif  // MEMRPC_CORE_SHM_LAYOUT_H_
