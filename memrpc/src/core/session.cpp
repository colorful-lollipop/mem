#include "core/session.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "virus_protection_service_log.h"

namespace MemRpc {

namespace {

constexpr uint32_t MAX_RING_ENTRIES = 1U << 20;
constexpr long LOCK_TIMEOUT_NS = 100L * 1000L * 1000L;
constexpr long NS_PER_SECOND = 1000L * 1000L * 1000L;

void CloseFd(int* fd)
{
    if (fd != nullptr && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

StatusCode LockSharedMutex(pthread_mutex_t* mutex)
{
    if (mutex == nullptr) {
        HILOGE("LockSharedMutex failed: mutex is null");
        return StatusCode::EngineInternalError;
    }

    timespec deadline{};
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        HILOGE("LockSharedMutex failed: clock_gettime errno=%{public}d", errno);
        return StatusCode::EngineInternalError;
    }
    deadline.tv_nsec += LOCK_TIMEOUT_NS;
    if (deadline.tv_nsec >= NS_PER_SECOND) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= NS_PER_SECOND;
    }

    const int rc = pthread_mutex_timedlock(mutex, &deadline);
    if (rc == 0) {
        return StatusCode::Ok;
    }
    if (rc == EOWNERDEAD) {
        HILOGW("LockSharedMutex observed owner death");
        pthread_mutex_consistent(mutex);
        pthread_mutex_unlock(mutex);
        return StatusCode::PeerDisconnected;
    }
    if (rc == ETIMEDOUT) {
        HILOGW("LockSharedMutex timed out");
        return StatusCode::PeerDisconnected;
    }
    if (rc == ENOTRECOVERABLE) {
        HILOGE("LockSharedMutex failed: mutex not recoverable");
        return StatusCode::PeerDisconnected;
    }
    HILOGE("LockSharedMutex failed: pthread_mutex_timedlock rc=%{public}d", rc);
    return StatusCode::EngineInternalError;
}

bool ValidateRingCursor(const RingCursor& cursor, uint32_t expected_capacity)
{
    if (expected_capacity == 0 || expected_capacity > MAX_RING_ENTRIES) {
        HILOGE("ValidateRingCursor failed: invalid expected_capacity=%{public}u", expected_capacity);
        return false;
    }
    if (cursor.capacity != expected_capacity) {
        HILOGE("ValidateRingCursor failed: cursor_capacity=%{public}u expected=%{public}u",
               cursor.capacity,
               expected_capacity);
        return false;
    }
    if (RingCount(cursor) > expected_capacity) {
        HILOGE("ValidateRingCursor failed: ring_count=%{public}u capacity=%{public}u",
               RingCount(cursor),
               expected_capacity);
        return false;
    }
    return true;
}

bool ValidateLayoutConfig(const LayoutConfig& config, std::size_t file_size)
{
    if (config.highRingSize == 0 || config.highRingSize > MAX_RING_ENTRIES) {
        HILOGE("ValidateLayoutConfig failed: invalid highRingSize=%{public}u", config.highRingSize);
        return false;
    }
    if (config.normalRingSize == 0 || config.normalRingSize > MAX_RING_ENTRIES) {
        HILOGE("ValidateLayoutConfig failed: invalid normalRingSize=%{public}u", config.normalRingSize);
        return false;
    }
    if (config.responseRingSize == 0 || config.responseRingSize > MAX_RING_ENTRIES) {
        HILOGE("ValidateLayoutConfig failed: invalid responseRingSize=%{public}u", config.responseRingSize);
        return false;
    }
    if (config.maxRequestBytes == 0 || config.maxResponseBytes == 0 ||
        !HasAlignedPayloadSizes(config.maxRequestBytes, config.maxResponseBytes)) {
        HILOGE("ValidateLayoutConfig failed: request=%{public}u response=%{public}u",
               config.maxRequestBytes,
               config.maxResponseBytes);
        return false;
    }

    const Layout layout = ComputeLayout(config);
    if (layout.totalSize < sizeof(SharedMemoryHeader) || layout.totalSize > file_size) {
        HILOGE("ValidateLayoutConfig failed: layout_size=%{public}zu file_size=%{public}zu",
               layout.totalSize,
               file_size);
        return false;
    }
    return true;
}

bool ProcessIsAlive(uint32_t pid_value)
{
    if (pid_value == 0) {
        return false;
    }
    const auto pid = static_cast<pid_t>(pid_value);
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

template <typename EntryType>
StatusCode PushRingEntry(Session::RingAccess access, const EntryType& entry)
{
    if (access.cursor == nullptr || access.entries == nullptr) {
        HILOGE("PushRingEntry failed: cursor=%{public}p entries=%{public}p", access.cursor, access.entries);
        return StatusCode::EngineInternalError;
    }
    const uint32_t head = access.cursor->head.load(std::memory_order_acquire);
    const uint32_t tail = access.cursor->tail.load(std::memory_order_relaxed);
    if (tail - head >= access.cursor->capacity) {
        HILOGW("PushRingEntry failed: queue full head=%{public}u tail=%{public}u capacity=%{public}u",
               head,
               tail,
               access.cursor->capacity);
        return StatusCode::QueueFull;
    }
    auto* entries = static_cast<EntryType*>(access.entries);
    entries[tail % access.cursor->capacity] = entry;
    access.cursor->tail.store(tail + 1U, std::memory_order_release);
    return StatusCode::Ok;
}

template <typename EntryType>
bool PopRingEntry(Session::RingAccess access, EntryType* entry)
{
    if (entry == nullptr || access.cursor == nullptr || access.entries == nullptr) {
        HILOGE("PopRingEntry failed: entry=%{public}p cursor=%{public}p entries=%{public}p",
               entry,
               access.cursor,
               access.entries);
        return false;
    }
    const uint32_t tail = access.cursor->tail.load(std::memory_order_acquire);
    const uint32_t head = access.cursor->head.load(std::memory_order_relaxed);
    if (tail == head) {
        return false;
    }
    auto* entries = static_cast<EntryType*>(access.entries);
    *entry = entries[head % access.cursor->capacity];
    access.cursor->head.store(head + 1U, std::memory_order_release);
    return true;
}

}  // namespace

Session::Session() = default;

Session::~Session()
{
    Reset();
}

StatusCode Session::MapAndValidateHeader(int shmFd)
{
    initialMappedSize_ = sizeof(SharedMemoryHeader);
    mappedRegion_ = mmap(nullptr, initialMappedSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (mappedRegion_ == MAP_FAILED) {
        HILOGE("Session::MapAndValidateHeader mmap failed: shmFd=%{public}d errno=%{public}d", shmFd, errno);
        mappedRegion_ = nullptr;
        return StatusCode::EngineInternalError;
    }

    header_ = static_cast<SharedMemoryHeader*>(mappedRegion_);
    if (header_->magic != SHARED_MEMORY_MAGIC || header_->protocolVersion != PROTOCOL_VERSION) {
        HILOGE("Session::MapAndValidateHeader failed: magic=%{public}u version=%{public}u expected_version=%{public}u",
               header_->magic,
               header_->protocolVersion,
               PROTOCOL_VERSION);
        Reset();
        return StatusCode::ProtocolMismatch;
    }
    return StatusCode::Ok;
}

StatusCode Session::RemapWithActualLayout(int shmFd)
{
    struct stat file_stat {};
    if (fstat(shmFd, &file_stat) != 0) {
        HILOGE("Session::RemapWithActualLayout fstat failed: shmFd=%{public}d errno=%{public}d", shmFd, errno);
        Reset();
        return StatusCode::EngineInternalError;
    }

    LayoutConfig config;
    config.highRingSize = header_->highRingSize;
    config.normalRingSize = header_->normalRingSize;
    config.responseRingSize = header_->responseRingSize;
    config.maxRequestBytes = header_->maxRequestBytes;
    config.maxResponseBytes = header_->maxResponseBytes;
    if (!ValidateLayoutConfig(config, static_cast<std::size_t>(file_stat.st_size)) ||
        !ValidateRingCursor(header_->highRing, config.highRingSize) ||
        !ValidateRingCursor(header_->normalRing, config.normalRingSize) ||
        !ValidateRingCursor(header_->responseRing, config.responseRingSize)) {
        HILOGE("Session::RemapWithActualLayout failed validation: session_id=%{public}llu",
               static_cast<unsigned long long>(header_->sessionId));
        Reset();
        return StatusCode::ProtocolMismatch;
    }
    Layout actual_layout = ComputeLayout(config);
    munmap(mappedRegion_, initialMappedSize_);

    mappedRegion_ = mmap(nullptr, actual_layout.totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (mappedRegion_ == MAP_FAILED) {
        HILOGE("Session::RemapWithActualLayout mmap failed: shmFd=%{public}d size=%{public}zu errno=%{public}d",
               shmFd,
               actual_layout.totalSize,
               errno);
        mappedRegion_ = nullptr;
        header_ = nullptr;
        return StatusCode::EngineInternalError;
    }

    mappedSize_ = actual_layout.totalSize;
    header_ = static_cast<SharedMemoryHeader*>(mappedRegion_);
    return StatusCode::Ok;
}

StatusCode Session::TryAcquireClientAttachment()
{
    const StatusCode lock_status = LockSharedMutex(&header_->clientStateMutex);
    if (lock_status != StatusCode::Ok) {
        HILOGE("Session::TryAcquireClientAttachment lock failed: status=%{public}d", static_cast<int>(lock_status));
        Reset();
        return lock_status;
    }
    if (header_->clientAttached != 0) {
        if (!ProcessIsAlive(header_->activeClientPid)) {
            HILOGW("Session::TryAcquireClientAttachment clearing stale client pid=%{public}u",
                   header_->activeClientPid);
            header_->clientAttached = 0;
            header_->activeClientPid = 0;
        } else {
            HILOGE("Session::TryAcquireClientAttachment failed: active client pid=%{public}u still alive",
                   header_->activeClientPid);
            pthread_mutex_unlock(&header_->clientStateMutex);
            Reset();
            return StatusCode::InvalidArgument;
        }
    }
    header_->clientAttached = 1;
    header_->activeClientPid = static_cast<uint32_t>(getpid());
    ownsClientAttachment_ = true;
    pthread_mutex_unlock(&header_->clientStateMutex);
    return StatusCode::Ok;
}

StatusCode Session::Attach(const BootstrapHandles& handles, AttachRole role)
{
    Reset();
    if (handles.shmFd < 0) {
        HILOGE("Session::Attach failed: invalid shmFd=%{public}d", handles.shmFd);
        return StatusCode::InvalidArgument;
    }

    StatusCode status = MapAndValidateHeader(handles.shmFd);
    if (status != StatusCode::Ok) {
        HILOGE("Session::Attach failed during MapAndValidateHeader: status=%{public}d", static_cast<int>(status));
        return status;
    }

    status = RemapWithActualLayout(handles.shmFd);
    if (status != StatusCode::Ok) {
        HILOGE("Session::Attach failed during RemapWithActualLayout: status=%{public}d", static_cast<int>(status));
        return status;
    }

    handles_ = handles;
    attachRole_ = role;
    ownsClientAttachment_ = false;
    if (role == AttachRole::Client) {
        status = TryAcquireClientAttachment();
        if (status != StatusCode::Ok) {
            HILOGE("Session::Attach failed during TryAcquireClientAttachment: status=%{public}d",
                   static_cast<int>(status));
            return status;
        }
    }
    return StatusCode::Ok;
}

void ReleaseClientAttachment(SharedMemoryHeader* header, bool owns_client_attachment)
{
    if (header == nullptr || !owns_client_attachment) {
        return;
    }
    if (LockSharedMutex(&header->clientStateMutex) != StatusCode::Ok) {
        HILOGW("ReleaseClientAttachment failed to lock clientStateMutex");
        return;
    }
    header->clientAttached = 0;
    header->activeClientPid = 0;
    pthread_mutex_unlock(&header->clientStateMutex);
}

void CloseHandles(BootstrapHandles* handles)
{
    if (handles == nullptr) {
        return;
    }
    CloseFd(&handles->shmFd);
    CloseFd(&handles->highReqEventFd);
    CloseFd(&handles->normalReqEventFd);
    CloseFd(&handles->respEventFd);
    CloseFd(&handles->reqCreditEventFd);
    CloseFd(&handles->respCreditEventFd);
}

void Session::Reset()
{
    ReleaseClientAttachment(header_, ownsClientAttachment_);
    if (mappedRegion_ != nullptr) {
        munmap(mappedRegion_, mappedSize_);
    }
    CloseHandles(&handles_);
    handles_ = MakeDefaultBootstrapHandles();
    mappedSize_ = 0;
    mappedRegion_ = nullptr;
    header_ = nullptr;
    attachRole_ = AttachRole::Server;
    ownsClientAttachment_ = false;
}

bool Session::Valid() const
{
    return header_ != nullptr;
}

const SharedMemoryHeader* Session::Header() const
{
    return header_;
}

SharedMemoryHeader* Session::mutableHeader()
{
    return header_;
}

const BootstrapHandles& Session::Handles() const
{
    return handles_;
}

Session::SessionState Session::State() const
{
    if (header_ == nullptr) {
        return SessionState::Broken;
    }
    return static_cast<SessionState>(header_->sessionState);
}

void Session::SetState(SessionState state)
{
    if (header_ != nullptr) {
        header_->sessionState = static_cast<uint32_t>(state);
    }
}

StatusCode Session::PushRequest(QueueKind queue, const RequestRingEntry& entry)
{
    return PushRingEntry<RequestRingEntry>(ResolveRing(queue), entry);
}

bool Session::PopRequest(QueueKind queue, RequestRingEntry* entry)
{
    return PopRingEntry<RequestRingEntry>(ResolveRing(queue), entry);
}

StatusCode Session::PushResponse(const ResponseRingEntry& entry)
{
    return PushRingEntry<ResponseRingEntry>(ResolveRing(QueueKind::Response), entry);
}

bool Session::PopResponse(ResponseRingEntry* entry)
{
    return PopRingEntry<ResponseRingEntry>(ResolveRing(QueueKind::Response), entry);
}

Session::RingAccess Session::ResolveRing(QueueKind queue)
{
    if (header_ == nullptr || mappedRegion_ == nullptr) {
        return {};
    }
    LayoutConfig config{header_->highRingSize,
                        header_->normalRingSize,
                        header_->responseRingSize,
                        header_->maxRequestBytes,
                        header_->maxResponseBytes};
    Layout layout = ComputeLayout(config);
    auto* base = static_cast<std::byte*>(mappedRegion_);
    switch (queue) {
        case QueueKind::HighRequest:
            return {&header_->highRing, base + layout.highRingOffset};
        case QueueKind::NormalRequest:
            return {&header_->normalRing, base + layout.normalRingOffset};
        case QueueKind::Response:
            return {&header_->responseRing, base + layout.responseRingOffset};
    }
    return {};
}

}  // namespace MemRpc
