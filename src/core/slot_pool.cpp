#include "core/slot_pool.h"

#include <cerrno>
#include <cstring>

namespace memrpc {

namespace {

bool IsQueuedState(SlotState state) {
  return state == SlotState::QueuedHigh || state == SlotState::QueuedNormal;
}

bool InitSharedMutex(pthread_mutex_t* mutex) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
  const int rc = pthread_mutex_init(mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  return rc == 0;
}

bool LockSharedMutex(pthread_mutex_t* mutex) {
  if (mutex == nullptr) {
    return false;
  }
  const int rc = pthread_mutex_lock(mutex);
  if (rc == 0) {
    return true;
  }
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(mutex);
    pthread_mutex_unlock(mutex);
  }
  return false;
}

}  // namespace

bool InitializeSharedSlotPool(void* region, uint32_t slot_count) {
  if (region == nullptr || slot_count == 0) {
    return false;
  }

  auto* header = static_cast<SharedSlotPoolHeader*>(region);
  std::memset(region, 0, ComputeSharedSlotPoolBytes(slot_count));
  if (!InitSharedMutex(&header->mutex)) {
    return false;
  }
  header->capacity = slot_count;
  header->available_count = slot_count;

  auto* free_slots =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader));
  for (uint32_t i = 0; i < slot_count; ++i) {
    free_slots[i] = slot_count - i - 1;
  }
  return true;
}

SharedSlotPool::SharedSlotPool(void* region) {
  if (region == nullptr) {
    return;
  }
  header_ = static_cast<SharedSlotPoolHeader*>(region);
  free_slots_ =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(region) + sizeof(SharedSlotPoolHeader));
}

std::optional<uint32_t> SharedSlotPool::Reserve() {
  if (!valid()) {
    return std::nullopt;
  }

  if (!LockSharedMutex(&header_->mutex)) {
    return std::nullopt;
  }
  if (header_->available_count == 0) {
    pthread_mutex_unlock(&header_->mutex);
    return std::nullopt;
  }

  const uint32_t slot_index = free_slots_[header_->available_count - 1];
  --header_->available_count;
  pthread_mutex_unlock(&header_->mutex);
  return slot_index;
}

bool SharedSlotPool::Release(uint32_t slot_index) {
  if (!valid()) {
    return false;
  }

  if (!LockSharedMutex(&header_->mutex)) {
    return false;
  }
  if (!IsValidIndex(slot_index) || header_->available_count >= header_->capacity) {
    pthread_mutex_unlock(&header_->mutex);
    return false;
  }
  free_slots_[header_->available_count] = slot_index;
  ++header_->available_count;
  pthread_mutex_unlock(&header_->mutex);
  return true;
}

uint32_t SharedSlotPool::capacity() const {
  return valid() ? header_->capacity : 0;
}

uint32_t SharedSlotPool::available() const {
  return valid() ? header_->available_count : 0;
}

bool SharedSlotPool::valid() const {
  return header_ != nullptr && free_slots_ != nullptr && header_->capacity != 0;
}

bool SharedSlotPool::IsValidIndex(uint32_t slot_index) const {
  return slot_index < header_->capacity;
}

SlotPool::SlotPool(uint32_t slot_count, uint32_t high_reserved_slots)
    : states_(slot_count, SlotState::Free),
      high_reserved_slots_(high_reserved_slots > slot_count ? slot_count : high_reserved_slots) {
  for (uint32_t i = 0; i < slot_count; ++i) {
    free_slots_.push(i);
  }
}

std::optional<uint32_t> SlotPool::Reserve(Priority priority) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_slots_.empty()) {
    return std::nullopt;
  }
  if (priority == Priority::Normal && free_slots_.size() <= high_reserved_slots_) {
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
