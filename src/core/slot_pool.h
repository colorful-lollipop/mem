#ifndef MEMRPC_CORE_SLOT_POOL_H_
#define MEMRPC_CORE_SLOT_POOL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "memrpc/core/types.h"

namespace memrpc {

enum class SlotState : uint8_t {
  Free = 0,
  Reserved,
  QueuedHigh,
  QueuedNormal,
  Dispatched,
  Processing,
  Responded,
};

struct SharedSlotPoolHeader {
  std::atomic<uint64_t> topTagged{0};  // {top_index:32, version:32} for ABA-safe CAS
  uint32_t capacity = 0;
  uint32_t padding = 0;
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "std::atomic<uint64_t> must be lock-free for cross-process Treiber stack");

inline std::size_t ComputeSharedSlotPoolBytes(uint32_t slot_count) {
  return sizeof(SharedSlotPoolHeader) + sizeof(uint32_t) * slot_count +
         sizeof(uint8_t) * slot_count;
}

bool InitializeSharedSlotPool(void* region, uint32_t slot_count);

class SharedSlotPool {
 public:
  explicit SharedSlotPool(void* region = nullptr);

  std::optional<uint32_t> Reserve();
  bool Release(uint32_t slot_index);
  uint32_t Capacity() const;
  uint32_t Available() const;
  bool Valid() const;

 private:
  bool IsValidIndex(uint32_t slot_index) const;

  SharedSlotPoolHeader* header_ = nullptr;
  uint32_t* freeSlots_ = nullptr;
  uint8_t* inUseSlots_ = nullptr;
};

// Lock-free slot pool for single-process use.
// Reserve is single-consumer (submit thread), Release is multi-producer
// (response thread + submit thread error paths). Implemented as a Treiber stack.
class SlotPool {
 public:
  explicit SlotPool(uint32_t slot_count);

  std::optional<uint32_t> Reserve();
  bool Transition(uint32_t slot_index, SlotState next_state);
  bool Release(uint32_t slot_index);
  SlotState GetState(uint32_t slot_index) const;
  uint32_t capacity() const;
  uint32_t available() const;

 private:
  static constexpr uint32_t EMPTY = UINT32_MAX;
  bool IsValidIndex(uint32_t slot_index) const;
  bool CanTransition(SlotState current, SlotState next) const;

  uint32_t slotCount_ = 0;
  std::vector<std::atomic<uint8_t>> states_;
  std::vector<uint32_t> nextFree_;
  std::atomic<uint32_t> freeTop_{EMPTY};
  std::atomic<uint32_t> freeCount_{0};
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_SLOT_POOL_H_
