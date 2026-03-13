#include "core/slot_pool.h"

#include <cstring>

namespace MemRpc {

namespace {

bool IsQueuedState(SlotState state) {
  return state == SlotState::QueuedHigh || state == SlotState::QueuedNormal;
}

constexpr uint32_t SHARED_EMPTY = UINT32_MAX;

uint64_t PackTagged(uint32_t index, uint32_t version) {
  return (static_cast<uint64_t>(version) << 32) | index;
}

uint32_t TaggedIndex(uint64_t tagged) {
  return static_cast<uint32_t>(tagged & 0xffffffffu);
}

uint32_t TaggedVersion(uint64_t tagged) {
  return static_cast<uint32_t>(tagged >> 32);
}

}  // namespace

bool InitializeSharedSlotPool(void* region, uint32_t slot_count) {
  if (region == nullptr || slot_count == 0) {
    return false;
  }

  auto* header = static_cast<SharedSlotPoolHeader*>(region);
  std::memset(region, 0, ComputeSharedSlotPoolBytes(slot_count));
  header->capacity = slot_count;

  // free_slots_ array is used as next-pointers for the Treiber stack.
  auto* next_ptrs =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader));
  auto* in_use_slots = reinterpret_cast<uint8_t*>(
      static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader) + sizeof(uint32_t) * slot_count);

  // Build linked list: 0 → 1 → 2 → ... → (n-1) → SHARED_EMPTY
  for (uint32_t i = 0; i < slot_count; ++i) {
    next_ptrs[i] = (i + 1 < slot_count) ? i + 1 : SHARED_EMPTY;
    in_use_slots[i] = 0;
  }
  // Stack top = 0, version = 0
  header->topTagged.store(PackTagged(0, 0), std::memory_order_release);
  return true;
}

SharedSlotPool::SharedSlotPool(void* region) {
  if (region == nullptr) {
    return;
  }
  header_ = static_cast<SharedSlotPoolHeader*>(region);
  freeSlots_ =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader));
  inUseSlots_ = reinterpret_cast<uint8_t*>(
      static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader) +
      sizeof(uint32_t) * header_->capacity);
}

std::optional<uint32_t> SharedSlotPool::Reserve() {
  if (!Valid()) {
    return std::nullopt;
  }

  uint64_t old_tagged = header_->topTagged.load(std::memory_order_acquire);
  while (true) {
    const uint32_t top = TaggedIndex(old_tagged);
    if (top == SHARED_EMPTY) {
      return std::nullopt;
    }
    if (!IsValidIndex(top) || inUseSlots_[top] != 0) {
      return std::nullopt;
    }
    const uint32_t next = freeSlots_[top];
    const uint32_t new_version = TaggedVersion(old_tagged) + 1;
    const uint64_t new_tagged = PackTagged(next, new_version);
    if (header_->topTagged.compare_exchange_weak(old_tagged, new_tagged,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
      inUseSlots_[top] = 1;
      return top;
    }
  }
}

bool SharedSlotPool::Release(uint32_t slot_index) {
  if (!Valid()) {
    return false;
  }
  if (!IsValidIndex(slot_index) || inUseSlots_[slot_index] == 0) {
    return false;
  }

  inUseSlots_[slot_index] = 0;

  uint64_t old_tagged = header_->topTagged.load(std::memory_order_relaxed);
  do {
    freeSlots_[slot_index] = TaggedIndex(old_tagged);
    const uint32_t new_version = TaggedVersion(old_tagged) + 1;
    const uint64_t new_tagged = PackTagged(slot_index, new_version);
    if (header_->topTagged.compare_exchange_weak(old_tagged, new_tagged,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
      return true;
    }
  } while (true);
}

uint32_t SharedSlotPool::Capacity() const {
  return Valid() ? header_->capacity : 0;
}

uint32_t SharedSlotPool::Available() const {
  if (!Valid()) {
    return 0;
  }
  // Walk the free list to count available slots.
  // This is O(n) but only used for stats/credit checks, not hot path.
  uint64_t tagged = header_->topTagged.load(std::memory_order_acquire);
  uint32_t count = 0;
  uint32_t idx = TaggedIndex(tagged);
  while (idx != SHARED_EMPTY && idx < header_->capacity) {
    ++count;
    idx = freeSlots_[idx];
  }
  return count;
}

bool SharedSlotPool::Valid() const {
  return header_ != nullptr && freeSlots_ != nullptr && inUseSlots_ != nullptr &&
         header_->capacity != 0;
}

bool SharedSlotPool::IsValidIndex(uint32_t slot_index) const {
  return slot_index < header_->capacity;
}

// --- SlotPool (lock-free Treiber stack) ---

SlotPool::SlotPool(uint32_t slot_count)
    : slotCount_(slot_count),
      states_(slot_count),
      nextFree_(slot_count) {
  for (uint32_t i = 0; i < slot_count; ++i) {
    states_[i].store(static_cast<uint8_t>(SlotState::Free), std::memory_order_relaxed);
  }
  // Build the free list as a linked stack: 0 → 1 → 2 → ... → (n-1) → EMPTY
  for (uint32_t i = 0; i < slot_count; ++i) {
    nextFree_[i] = (i + 1 < slot_count) ? i + 1 : EMPTY;
  }
  freeTop_.store(slot_count > 0 ? 0 : EMPTY, std::memory_order_relaxed);
  freeCount_.store(slot_count, std::memory_order_relaxed);
}

std::optional<uint32_t> SlotPool::Reserve() {
  const uint32_t count = freeCount_.load(std::memory_order_acquire);
  if (count == 0) {
    return std::nullopt;
  }

  // Pop from the Treiber stack. Single consumer (submit thread), so ABA is not possible.
  uint32_t top = freeTop_.load(std::memory_order_acquire);
  while (top != EMPTY) {
    const uint32_t next = nextFree_[top];
    if (freeTop_.compare_exchange_weak(top, next,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      states_[top].store(static_cast<uint8_t>(SlotState::Reserved), std::memory_order_relaxed);
      freeCount_.fetch_sub(1, std::memory_order_release);
      return top;
    }
    // CAS failed — a concurrent Release changed top, retry.
  }
  return std::nullopt;
}

bool SlotPool::Transition(uint32_t slot_index, SlotState next_state) {
  if (!IsValidIndex(slot_index)) {
    return false;
  }

  const auto current = static_cast<SlotState>(states_[slot_index].load(std::memory_order_relaxed));
  if (!CanTransition(current, next_state)) {
    return false;
  }

  states_[slot_index].store(static_cast<uint8_t>(next_state), std::memory_order_relaxed);
  return true;
}

bool SlotPool::Release(uint32_t slot_index) {
  if (!IsValidIndex(slot_index)) {
    return false;
  }

  const auto current = static_cast<SlotState>(states_[slot_index].load(std::memory_order_relaxed));
  if (current == SlotState::Free) {
    return false;
  }

  states_[slot_index].store(static_cast<uint8_t>(SlotState::Free), std::memory_order_relaxed);

  // Push onto the Treiber stack (lock-free, safe for concurrent pushers).
  uint32_t top = freeTop_.load(std::memory_order_relaxed);
  do {
    nextFree_[slot_index] = top;
  } while (!freeTop_.compare_exchange_weak(top, slot_index,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));
  freeCount_.fetch_add(1, std::memory_order_release);
  return true;
}

SlotState SlotPool::GetState(uint32_t slot_index) const {
  if (!IsValidIndex(slot_index)) {
    return SlotState::Free;
  }
  return static_cast<SlotState>(states_[slot_index].load(std::memory_order_relaxed));
}

uint32_t SlotPool::capacity() const {
  return slotCount_;
}

uint32_t SlotPool::available() const {
  return freeCount_.load(std::memory_order_relaxed);
}

bool SlotPool::IsValidIndex(uint32_t slot_index) const {
  return slot_index < slotCount_;
}

bool SlotPool::CanTransition(SlotState current, SlotState next) const {
  switch (current) {
    case SlotState::Free:
      return false;
    case SlotState::Reserved:
      return IsQueuedState(next);
    case SlotState::QueuedHigh:
    case SlotState::QueuedNormal:
      return next == SlotState::Dispatched;
    case SlotState::Dispatched:
      return next == SlotState::Processing;
    case SlotState::Processing:
      return next == SlotState::Responded;
    case SlotState::Responded:
      return false;
  }
  return false;
}

}  // namespace MemRpc
