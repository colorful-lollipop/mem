#ifndef MEMRPC_CORE_RING_BUFFER_H_
#define MEMRPC_CORE_RING_BUFFER_H_

#include <cstddef>

namespace memrpc {

template <typename T>
class RingBuffer {
 public:
  RingBuffer(T* storage, std::size_t capacity)
      : storage_(storage), capacity_(capacity) {}

  bool Push(const T& value) {
    if (Full()) {
      return false;
    }
    storage_[tail_] = value;
    tail_ = NextIndex(tail_);
    ++size_;
    return true;
  }

  bool Pop(T* value) {
    if (value == nullptr || Empty()) {
      return false;
    }
    *value = storage_[head_];
    head_ = NextIndex(head_);
    --size_;
    return true;
  }

  bool Empty() const {
    return size_ == 0;
  }

  bool Full() const {
    return size_ == capacity_;
  }

  std::size_t Size() const {
    return size_;
  }

  std::size_t Capacity() const {
    return capacity_;
  }

 private:
  std::size_t NextIndex(std::size_t index) const {
    return (index + 1) % capacity_;
  }

  T* storage_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t size_ = 0;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_RING_BUFFER_H_
