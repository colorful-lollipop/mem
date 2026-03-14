#ifndef MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_
#define MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_

#include <cerrno>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <atomic>

#include "memrpc/core/protocol.h"

namespace MemRpc {

constexpr short kPollFailureEvents = POLLERR | POLLHUP | POLLNVAL;

enum class PollEventFdResult : uint8_t { Ready, Timeout, Retry, Failed };

template <typename F>
class ScopeExit final {
 public:
  explicit ScopeExit(F fn) : fn_(std::move(fn)) {}

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  ScopeExit(ScopeExit&& other) noexcept
      : fn_(std::move(other.fn_)), active_(other.active_) {
    other.active_ = false;
  }

  ScopeExit& operator=(ScopeExit&&) = delete;

  ~ScopeExit() {
    if (active_) {
      fn_();
    }
  }

 private:
  F fn_;
  bool active_ = true;
};

template <typename F>
[[nodiscard]] auto MakeScopeExit(F&& fn) {
  return ScopeExit<std::decay_t<F>>(std::forward<F>(fn));
}

[[nodiscard]] inline uint64_t MonotonicNowMs64() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

[[nodiscard]] inline uint32_t MonotonicNowMs() {
  return static_cast<uint32_t>(MonotonicNowMs64() & 0xffffffffU);
}

[[nodiscard]] inline bool DeadlineReached(
    std::chrono::steady_clock::time_point deadline) {
  return std::chrono::steady_clock::now() >= deadline;
}

template <typename Cursor>
[[nodiscard]] inline bool RingCountIsOneAfterPush(const Cursor& cursor) {
  const uint32_t tail = cursor.tail.load(std::memory_order_acquire);
  const uint32_t head = cursor.head.load(std::memory_order_acquire);
  return tail - head == 1U;
}

[[nodiscard]] inline int64_t RemainingTimeoutMs(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return std::numeric_limits<int64_t>::max();
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now())
          .count();
  return remaining > 0 ? remaining : 0;
}

[[nodiscard]] inline bool DrainEventFd(int fd) {
  bool drained = false;
  while (true) {
    uint64_t counter = 0;
    const ssize_t read_bytes = read(fd, &counter, sizeof(counter));
    if (read_bytes == sizeof(counter)) {
      drained = true;
      continue;
    }
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return drained;
    }
    return false;
  }
}

[[nodiscard]] inline bool SignalEventFd(int fd) {
  if (fd < 0) {
    return false;
  }
  const uint64_t signal_value = 1;
  while (true) {
    const ssize_t write_bytes = write(fd, &signal_value, sizeof(signal_value));
    if (write_bytes == sizeof(signal_value)) {
      return true;
    }
    if (write_bytes < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

[[nodiscard]] inline PollEventFdResult PollEventFd(pollfd* fd, int timeout_ms) {
  if (fd == nullptr) {
    return PollEventFdResult::Failed;
  }
  fd->revents = 0;
  const int poll_result = poll(fd, 1, timeout_ms);
  if (poll_result == 0) {
    return PollEventFdResult::Timeout;
  }
  if (poll_result < 0) {
    return errno == EINTR ? PollEventFdResult::Retry : PollEventFdResult::Failed;
  }
  if ((fd->revents & kPollFailureEvents) != 0) {
    return PollEventFdResult::Failed;
  }
  if ((fd->revents & POLLIN) == 0) {
    return PollEventFdResult::Retry;
  }
  return DrainEventFd(fd->fd) ? PollEventFdResult::Ready : PollEventFdResult::Retry;
}

inline void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

}  // namespace MemRpc

#endif  // MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_
