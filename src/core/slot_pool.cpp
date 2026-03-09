#include "core/slot_pool.h"

namespace memrpc {

namespace {

bool IsQueuedState(SlotState state) {
  return state == SlotState::QueuedHigh || state == SlotState::QueuedNormal;
}

}  // namespace

SlotPool::SlotPool(uint32_t slot_count) : states_(slot_count, SlotState::Free) {
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
  states_[slot_index] = SlotState::Reserved;
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

  if (states_[slot_index] == SlotState::Free) {
    return false;
  }

  states_[slot_index] = SlotState::Free;
  free_slots_.push(slot_index);
  return true;
}

SlotState SlotPool::GetState(uint32_t slot_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsValidIndex(slot_index)) {
    return SlotState::Free;
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

}  // namespace memrpc
