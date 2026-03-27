#ifndef MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_
#define MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_

#include <poll.h>
#include <unistd.h>
#include <cerrno>

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "memrpc/core/protocol.h"

namespace MemRpc {

constexpr short kPollFailureEvents = POLLERR | POLLHUP | POLLNVAL;

enum class PollEventFdResult : uint8_t { Ready, Timeout, Retry, Failed };
enum class EventFdReadResult : uint8_t { Consumed, Retry, Empty, Failed };
enum class EventFdWriteResult : uint8_t { Wrote, Retry, Failed };

template <typename F>
class ScopeExit final {
public:
    explicit ScopeExit(F fn)
        : fn_(std::move(fn))
    {
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept
        : fn_(std::move(other.fn_)),
          active_(other.active_)
    {
        other.active_ = false;
    }

    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit()
    {
        if (active_) {
            fn_();
        }
    }

private:
    F fn_;
    bool active_ = true;
};

template <typename F>
[[nodiscard]] auto MakeScopeExit(F&& fn)
{
    return ScopeExit<std::decay_t<F>>(std::forward<F>(fn));
}

[[nodiscard]] inline uint64_t MonotonicNowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

inline void CpuRelax();

[[nodiscard]] inline uint32_t AtomicLoadUint32(const uint32_t& value_ref)
{
    return __atomic_load_n(&value_ref, __ATOMIC_ACQUIRE);
}

inline void AtomicStoreUint32(uint32_t& value_ref, uint32_t value)
{
    __atomic_store_n(&value_ref, value, __ATOMIC_RELEASE);
}

[[nodiscard]] inline uint64_t AtomicLoadUint64(const uint64_t& value_ref)
{
    return __atomic_load_n(&value_ref, __ATOMIC_ACQUIRE);
}

inline void AtomicStoreUint64(uint64_t& value_ref, uint64_t value)
{
    __atomic_store_n(&value_ref, value, __ATOMIC_RELEASE);
}

[[nodiscard]] inline bool DeadlineReached(std::chrono::steady_clock::time_point deadline)
{
    return std::chrono::steady_clock::now() >= deadline;
}

template <typename Cursor>
[[nodiscard]] inline bool RingCountIsOneAfterPush(const Cursor& cursor)
{
    const uint32_t tail = cursor.tail.load(std::memory_order_acquire);
    const uint32_t head = cursor.head.load(std::memory_order_acquire);
    return tail - head == 1U;
}

[[nodiscard]] inline int64_t RemainingTimeoutMs(std::chrono::steady_clock::time_point deadline)
{
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::numeric_limits<int64_t>::max();
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return remaining > 0 ? remaining : 0;
}

[[nodiscard]] inline EventFdReadResult ReadEventFdOnce(int fd)
{
    uint64_t counter = 0;
    const ssize_t readBytes = read(fd, &counter, sizeof(counter));
    if (readBytes == sizeof(counter)) return EventFdReadResult::Consumed;
    if (readBytes < 0 && errno == EINTR) return EventFdReadResult::Retry;
    if (readBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return EventFdReadResult::Empty;
    return EventFdReadResult::Failed;
}

[[nodiscard]] inline bool DrainEventFd(int fd)
{
    bool drained = false;
    while (true) {
        const auto result = ReadEventFdOnce(fd);
        if (result == EventFdReadResult::Consumed) { drained = true; continue; }
        if (result == EventFdReadResult::Retry) { continue; }
        return result == EventFdReadResult::Empty ? drained : false;
    }
}

[[nodiscard]] inline EventFdWriteResult WriteEventFdOnce(int fd)
{
    constexpr uint64_t signalValue = 1;
    const ssize_t writeBytes = write(fd, &signalValue, sizeof(signalValue));
    if (writeBytes == sizeof(signalValue)) return EventFdWriteResult::Wrote;
    return writeBytes < 0 && errno == EINTR ? EventFdWriteResult::Retry : EventFdWriteResult::Failed;
}

[[nodiscard]] inline bool SignalEventFd(int fd)
{
    if (fd < 0) return false;
    while (true) {
        const auto result = WriteEventFdOnce(fd);
        if (result == EventFdWriteResult::Wrote) return true;
        if (result == EventFdWriteResult::Failed) return false;
    }
}

[[nodiscard]] inline PollEventFdResult ClassifyPollCallResult(int pollResult)
{
    if (pollResult == 0) return PollEventFdResult::Timeout;
    if (pollResult < 0) return errno == EINTR ? PollEventFdResult::Retry : PollEventFdResult::Failed;
    return PollEventFdResult::Ready;
}

[[nodiscard]] inline PollEventFdResult HandlePolledEvent(const pollfd& fd)
{
    if ((fd.revents & kPollFailureEvents) != 0) return PollEventFdResult::Failed;
    if ((fd.revents & POLLIN) == 0) return PollEventFdResult::Retry;
    return DrainEventFd(fd.fd) ? PollEventFdResult::Ready : PollEventFdResult::Retry;
}

[[nodiscard]] inline PollEventFdResult PollEventFd(pollfd* fd, int timeout_ms)
{
    if (fd == nullptr) {
        return PollEventFdResult::Failed;
    }
    fd->revents = 0;
    const auto pollResult = ClassifyPollCallResult(poll(fd, 1, timeout_ms));
    return pollResult == PollEventFdResult::Ready ? HandlePolledEvent(*fd) : pollResult;
}

inline void CpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

}  // namespace MemRpc

#endif  // MEMRPC_INCLUDE_CORE_RUNTIME_UTILS_H_
