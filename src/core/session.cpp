#include "core/session.h"

#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <cstring>

namespace memrpc {

namespace {

constexpr uint32_t MAX_RING_ENTRIES = 1u << 20;
constexpr uint32_t MAX_SLOT_COUNT = 1u << 20;

StatusCode LockSharedMutex(pthread_mutex_t* mutex) {
  if (mutex == nullptr) {
    return StatusCode::EngineInternalError;
  }

  timespec deadline {};
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return StatusCode::EngineInternalError;
  }
  deadline.tv_nsec += 100 * 1000 * 1000;
  if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000 * 1000 * 1000;
  }

  const int rc = pthread_mutex_timedlock(mutex, &deadline);
  if (rc == 0) {
    return StatusCode::Ok;
  }
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(mutex);
    pthread_mutex_unlock(mutex);
    return StatusCode::PeerDisconnected;
  }
  if (rc == ETIMEDOUT) {
    return StatusCode::PeerDisconnected;
  }
  if (rc == ENOTRECOVERABLE) {
    return StatusCode::PeerDisconnected;
  }
  return StatusCode::EngineInternalError;
}

bool ValidateRingCursor(const RingCursor& cursor, uint32_t expected_capacity) {
  if (expected_capacity == 0 || expected_capacity > MAX_RING_ENTRIES) {
    return false;
  }
  if (cursor.capacity != expected_capacity) {
    return false;
  }
  if (RingCount(cursor) > expected_capacity) {
    return false;
  }
  return true;
}

bool ValidateLayoutConfig(const LayoutConfig& config, std::size_t file_size) {
  if (config.highRingSize == 0 || config.highRingSize > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.normalRingSize == 0 || config.normalRingSize > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.responseRingSize == 0 || config.responseRingSize > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.slotCount == 0 || config.slotCount > MAX_SLOT_COUNT) {
    return false;
  }
  if (config.maxRequestBytes == 0 || config.maxResponseBytes == 0 ||
      config.maxResponseBytes > DEFAULT_MAX_RESPONSE_BYTES) {
    return false;
  }
  if (config.slotSize != ComputeSlotSize(config.maxRequestBytes, config.maxResponseBytes)) {
    return false;
  }

  const Layout layout = ComputeLayout(config);
  if (layout.totalSize < sizeof(SharedMemoryHeader) || layout.totalSize > file_size) {
    return false;
  }
  return true;
}

bool ProcessIsAlive(uint32_t pid_value) {
  if (pid_value == 0) {
    return false;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  if (kill(pid, 0) == 0) {
    return true;
  }
  return errno != ESRCH;
}

template <typename EntryType>
StatusCode PushRingEntry(Session::RingAccess access, const EntryType& entry) {
  if (access.cursor == nullptr || access.entries == nullptr) {
    return StatusCode::EngineInternalError;
  }
  const uint32_t head = access.cursor->head.load(std::memory_order_acquire);
  const uint32_t tail = access.cursor->tail.load(std::memory_order_relaxed);
  if (tail - head >= access.cursor->capacity) {
    return StatusCode::QueueFull;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  entries[tail % access.cursor->capacity] = entry;
  access.cursor->tail.store(tail + 1u, std::memory_order_release);
  return StatusCode::Ok;
}

template <typename EntryType>
bool PopRingEntry(Session::RingAccess access, EntryType* entry) {
  if (entry == nullptr || access.cursor == nullptr || access.entries == nullptr) {
    return false;
  }
  const uint32_t tail = access.cursor->tail.load(std::memory_order_acquire);
  const uint32_t head = access.cursor->head.load(std::memory_order_relaxed);
  if (tail == head) {
    return false;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  *entry = entries[head % access.cursor->capacity];
  access.cursor->head.store(head + 1u, std::memory_order_release);
  return true;
}

}  // namespace

Session::Session() = default;

Session::~Session() {
  Reset();
}

StatusCode Session::MapAndValidateHeader(int shmFd) {
  LayoutConfig config;
  config.highRingSize = 32;
  config.normalRingSize = 32;
  config.responseRingSize = 64;
  config.slotCount = 64;
  config.slotSize = ComputeSlotSize(DEFAULT_MAX_REQUEST_BYTES, DEFAULT_MAX_RESPONSE_BYTES);
  config.maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  config.maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;

  Layout default_layout = ComputeLayout(config);
  initialMappedSize_ = default_layout.totalSize;
  mappedRegion_ =
      mmap(nullptr, default_layout.totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
  if (mappedRegion_ == MAP_FAILED) {
    mappedRegion_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  header_ = static_cast<SharedMemoryHeader*>(mappedRegion_);
  if (header_->magic != SHARED_MEMORY_MAGIC || header_->protocolVersion != PROTOCOL_VERSION) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }
  return StatusCode::Ok;
}

StatusCode Session::RemapWithActualLayout(int shmFd) {
  struct stat file_stat {};
  if (fstat(shmFd, &file_stat) != 0) {
    Reset();
    return StatusCode::EngineInternalError;
  }

  LayoutConfig config;
  config.highRingSize = header_->highRingSize;
  config.normalRingSize = header_->normalRingSize;
  config.responseRingSize = header_->responseRingSize;
  config.slotCount = header_->slotCount;
  config.slotSize = header_->slotSize;
  config.maxRequestBytes = header_->maxRequestBytes;
  config.maxResponseBytes = header_->maxResponseBytes;
  if (!ValidateLayoutConfig(config, static_cast<std::size_t>(file_stat.st_size)) ||
      !ValidateRingCursor(header_->highRing, config.highRingSize) ||
      !ValidateRingCursor(header_->normalRing, config.normalRingSize) ||
      !ValidateRingCursor(header_->responseRing, config.responseRingSize)) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }
  Layout actual_layout = ComputeLayout(config);
  munmap(mappedRegion_, initialMappedSize_);

  mappedRegion_ =
      mmap(nullptr, actual_layout.totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
  if (mappedRegion_ == MAP_FAILED) {
    mappedRegion_ = nullptr;
    header_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  mappedSize_ = actual_layout.totalSize;
  header_ = static_cast<SharedMemoryHeader*>(mappedRegion_);
  return StatusCode::Ok;
}

StatusCode Session::TryAcquireClientSlot() {
  const StatusCode lock_status = LockSharedMutex(&header_->clientStateMutex);
  if (lock_status != StatusCode::Ok) {
    Reset();
    return lock_status;
  }
  if (header_->clientAttached != 0) {
    if (!ProcessIsAlive(header_->activeClientPid)) {
      header_->clientAttached = 0;
      header_->activeClientPid = 0;
    } else {
      pthread_mutex_unlock(&header_->clientStateMutex);
      Reset();
      return StatusCode::InvalidArgument;
    }
  }
  header_->clientAttached = 1;
  header_->activeClientPid = static_cast<uint32_t>(getpid());
  ownsClientSlot_ = true;
  pthread_mutex_unlock(&header_->clientStateMutex);
  return StatusCode::Ok;
}

StatusCode Session::Attach(const BootstrapHandles& handles, AttachRole role) {
  Reset();
  if (handles.shmFd < 0) {
    return StatusCode::InvalidArgument;
  }

  StatusCode status = MapAndValidateHeader(handles.shmFd);
  if (status != StatusCode::Ok) {
    return status;
  }

  status = RemapWithActualLayout(handles.shmFd);
  if (status != StatusCode::Ok) {
    return status;
  }

  handles_ = handles;
  attachRole_ = role;
  ownsClientSlot_ = false;
  if (role == AttachRole::Client) {
    status = TryAcquireClientSlot();
    if (status != StatusCode::Ok) {
      return status;
    }
  }
  return StatusCode::Ok;
}

void Session::Reset() {
  if (header_ != nullptr && ownsClientSlot_) {
    if (LockSharedMutex(&header_->clientStateMutex) == StatusCode::Ok) {
      header_->clientAttached = 0;
      header_->activeClientPid = 0;
      pthread_mutex_unlock(&header_->clientStateMutex);
    }
  }
  if (mappedRegion_ != nullptr) {
    munmap(mappedRegion_, mappedSize_);
  }
  if (handles_.shmFd >= 0) {
    close(handles_.shmFd);
  }
  if (handles_.highReqEventFd >= 0) {
    close(handles_.highReqEventFd);
  }
  if (handles_.normalReqEventFd >= 0) {
    close(handles_.normalReqEventFd);
  }
  if (handles_.respEventFd >= 0) {
    close(handles_.respEventFd);
  }
  if (handles_.reqCreditEventFd >= 0) {
    close(handles_.reqCreditEventFd);
  }
  if (handles_.respCreditEventFd >= 0) {
    close(handles_.respCreditEventFd);
  }
  mappedSize_ = 0;
  mappedRegion_ = nullptr;
  header_ = nullptr;
  handles_ = {};
  attachRole_ = AttachRole::Server;
  ownsClientSlot_ = false;
}

bool Session::Valid() const {
  return header_ != nullptr;
}

const SharedMemoryHeader* Session::Header() const {
  return header_;
}

SharedMemoryHeader* Session::mutableHeader() {
  return header_;
}

const BootstrapHandles& Session::Handles() const {
  return handles_;
}

Session::SessionState Session::State() const {
  if (header_ == nullptr) {
    return SessionState::Broken;
  }
  return static_cast<SessionState>(header_->sessionState);
}

void Session::SetState(SessionState state) {
  if (header_ != nullptr) {
    header_->sessionState = static_cast<uint32_t>(state);
  }
}

SlotPayload* Session::GetSlotPayload(uint32_t slot_index) {
  if (header_ == nullptr || slot_index >= header_->slotCount) {
    return nullptr;
  }
  LayoutConfig config{header_->highRingSize,
                      header_->normalRingSize,
                      header_->responseRingSize,
                      header_->slotCount,
                      header_->slotSize,
                      header_->maxRequestBytes,
                      header_->maxResponseBytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mappedRegion_);
  return reinterpret_cast<SlotPayload*>(base + layout.slotPoolOffset +
                                        static_cast<std::size_t>(slot_index) * header_->slotSize);
}

uint8_t* Session::GetSlotRequestBytes(uint32_t slot_index) {
  SlotPayload* payload = GetSlotPayload(slot_index);
  if (payload == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<uint8_t*>(payload);
  return base + sizeof(SlotPayload);
}

ResponseSlotPayload* Session::GetResponseSlotPayload(uint32_t slot_index) {
  if (header_ == nullptr || slot_index >= header_->responseRingSize) {
    return nullptr;
  }
  LayoutConfig config{header_->highRingSize,
                      header_->normalRingSize,
                      header_->responseRingSize,
                      header_->slotCount,
                      header_->slotSize,
                      header_->maxRequestBytes,
                      header_->maxResponseBytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mappedRegion_);
  return reinterpret_cast<ResponseSlotPayload*>(
      base + layout.responseSlotsOffset +
      static_cast<std::size_t>(slot_index) * ComputeResponseSlotSize(header_->maxResponseBytes));
}

uint8_t* Session::GetResponseSlotBytes(uint32_t slot_index) {
  ResponseSlotPayload* payload = GetResponseSlotPayload(slot_index);
  if (payload == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<uint8_t*>(payload);
  return base + sizeof(ResponseSlotPayload);
}

void* Session::GetResponseSlotPoolRegion() {
  if (header_ == nullptr || mappedRegion_ == nullptr) {
    return nullptr;
  }
  LayoutConfig config{header_->highRingSize,
                      header_->normalRingSize,
                      header_->responseRingSize,
                      header_->slotCount,
                      header_->slotSize,
                      header_->maxRequestBytes,
                      header_->maxResponseBytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mappedRegion_);
  return base + layout.responseSlotPoolOffset;
}

StatusCode Session::PushRequest(QueueKind queue, const RequestRingEntry& entry) {
  return PushRingEntry<RequestRingEntry>(ResolveRing(queue), entry);
}

bool Session::PopRequest(QueueKind queue, RequestRingEntry* entry) {
  return PopRingEntry<RequestRingEntry>(ResolveRing(queue), entry);
}

StatusCode Session::PushResponse(const ResponseRingEntry& entry) {
  return PushRingEntry<ResponseRingEntry>(ResolveRing(QueueKind::Response), entry);
}

bool Session::PopResponse(ResponseRingEntry* entry) {
  return PopRingEntry<ResponseRingEntry>(ResolveRing(QueueKind::Response), entry);
}

Session::RingAccess Session::ResolveRing(QueueKind queue) {
  if (header_ == nullptr || mappedRegion_ == nullptr) {
    return {};
  }
  LayoutConfig config{header_->highRingSize,
                      header_->normalRingSize,
                      header_->responseRingSize,
                      header_->slotCount,
                      header_->slotSize,
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

}  // namespace memrpc
