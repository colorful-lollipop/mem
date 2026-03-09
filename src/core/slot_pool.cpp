#include "core/slot_pool.h"

namespace memrpc {

namespace {

bool IsQueuedState(SlotState state) {
  return state == SlotState::kQueuedHigh || state == SlotState::kQueuedNormal;
}

}  // namespace

SlotPool::SlotPool(uint32_t slot_count) : states_(slot_count, SlotState::kFree) {
  for (uint32_t i = 0; i < slot_count; ++i) {
    free_slots_.push(i);
  }
}

std::optional<uint32_t> SlotPool::Reserve() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_slots_.empty()) {
    return std::nullopt;
  }

  const uint32_t slot_index = free_slots_.front();
  free_slots_.pop();
  states_[slot_index] = SlotState::kReserved;
  return slot_index;
}

bool SlotPool::Transition(uint32_t slot_index, SlotState next_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsValidIndex(slot_index)) {
    return false;
  }

  const SlotState current = states_[slot_index];
  if (!CanTransition(current, next_state)) {
    return false;
  }

  states_[slot_index] = next_state;
  return true;
}

bool SlotPool::Release(uint32_t slot_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsValidIndex(slot_index)) {
    return false;
  }

  if (states_[slot_index] == SlotState::kFree) {
    return false;
  }

  states_[slot_index] = SlotState::kFree;
  free_slots_.push(slot_index);
  return true;
}

SlotState SlotPool::GetState(uint32_t slot_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsValidIndex(slot_index)) {
    return SlotState::kFree;
  }
  return states_[slot_index];
}

uint32_t SlotPool::capacity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(states_.size());
}

bool SlotPool::IsValidIndex(uint32_t slot_index) const {
  return slot_index < states_.size();
}

bool SlotPool::CanTransition(SlotState current, SlotState next) const {
  switch (current) {
    case SlotState::kFree:
      return false;
    case SlotState::kReserved:
      return IsQueuedState(next);
    case SlotState::kQueuedHigh:
    case SlotState::kQueuedNormal:
      return next == SlotState::kDispatched;
    case SlotState::kDispatched:
      return next == SlotState::kProcessing;
    case SlotState::kProcessing:
      return next == SlotState::kResponded;
    case SlotState::kResponded:
      return false;
  }
  return false;
}

}  // namespace memrpc
