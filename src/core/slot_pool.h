#ifndef MEMRPC_CORE_SLOT_POOL_H_
#define MEMRPC_CORE_SLOT_POOL_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <queue>
#include <vector>

#include "memrpc/core/types.h"

namespace memrpc {

enum class SlotState {
  Free = 0,
  Reserved,
  QueuedHigh,
  QueuedNormal,
  Dispatched,
  Processing,
  Responded,
};

struct SharedSlotPoolHeader {
  pthread_mutex_t mutex{};
  uint32_t capacity = 0;
  uint32_t available_count = 0;
  uint32_t reserved[2]{};
};

inline std::size_t ComputeSharedSlotPoolBytes(uint32_t slot_count) {
  return sizeof(SharedSlotPoolHeader) + sizeof(uint32_t) * slot_count;
}

bool InitializeSharedSlotPool(void* region, uint32_t slot_count);

class SharedSlotPool {
 public:
  explicit SharedSlotPool(void* region = nullptr);

  std::optional<uint32_t> Reserve();
  bool Release(uint32_t slot_index);
  uint32_t capacity() const;
  uint32_t available() const;
  bool valid() const;

 private:
  bool IsValidIndex(uint32_t slot_index) const;

  SharedSlotPoolHeader* header_ = nullptr;
  uint32_t* free_slots_ = nullptr;
};

class SlotPool {
 public:
  explicit SlotPool(uint32_t slot_count, uint32_t high_reserved_slots = 0);

  // Reserve/Transition/Release 负责维护 slot 生命周期，避免请求和回包交叉踩踏。
  std::optional<uint32_t> Reserve(Priority priority = Priority::Normal);
  bool Transition(uint32_t slot_index, SlotState next_state);
  bool Release(uint32_t slot_index);
  SlotState GetState(uint32_t slot_index) const;
  uint32_t capacity() const;

 private:
  bool IsValidIndex(uint32_t slot_index) const;
  bool CanTransition(SlotState current, SlotState next) const;

  std::vector<SlotState> states_;
  std::queue<uint32_t> free_slots_;
  uint32_t high_reserved_slots_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_SLOT_POOL_H_
