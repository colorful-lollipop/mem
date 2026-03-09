#ifndef MEMRPC_CORE_SLOT_POOL_H_
#define MEMRPC_CORE_SLOT_POOL_H_

#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace memrpc {

enum class SlotState {
  kFree = 0,
  kReserved,
  kQueuedHigh,
  kQueuedNormal,
  kDispatched,
  kProcessing,
  kResponded,
};

class SlotPool {
 public:
  explicit SlotPool(uint32_t slot_count);

  std::optional<uint32_t> Reserve();
  bool Transition(uint32_t slot_index, SlotState next_state);
  bool Release(uint32_t slot_index);
  SlotState GetState(uint32_t slot_index) const;
  uint32_t capacity() const;

 private:
  bool IsValidIndex(uint32_t slot_index) const;
  bool CanTransition(SlotState current, SlotState next) const;

  std::vector<SlotState> states_;
  std::queue<uint32_t> free_slots_;
  mutable std::mutex mutex_;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_SLOT_POOL_H_
