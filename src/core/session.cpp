#include "core/session.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <cstring>

namespace memrpc {

namespace {

constexpr uint32_t kMaxRingEntries = 1u << 20;
constexpr uint32_t kMaxSlotCount = 1u << 20;

StatusCode LockRingMutex(pthread_mutex_t* mutex) {
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
  if (expected_capacity == 0 || expected_capacity > kMaxRingEntries) {
    return false;
  }
  if (cursor.capacity != expected_capacity) {
    return false;
  }
  if (cursor.head >= expected_capacity || cursor.tail >= expected_capacity) {
    return false;
  }
  if (cursor.size > expected_capacity) {
    return false;
  }
  return true;
}

bool ValidateLayoutConfig(const LayoutConfig& config, std::size_t file_size) {
  if (config.high_ring_size == 0 || config.high_ring_size > kMaxRingEntries) {
    return false;
  }
  if (config.normal_ring_size == 0 || config.normal_ring_size > kMaxRingEntries) {
    return false;
  }
  if (config.response_ring_size == 0 || config.response_ring_size > kMaxRingEntries) {
    return false;
  }
  if (config.slot_count == 0 || config.slot_count > kMaxSlotCount) {
    return false;
  }
  if (config.slot_size != sizeof(SlotPayload)) {
    return false;
  }

  const Layout layout = ComputeLayout(config);
  if (layout.total_size < sizeof(SharedMemoryHeader) || layout.total_size > file_size) {
    return false;
  }
  return true;
}

template <typename EntryType>
StatusCode PushRingEntry(Session::RingAccess access, const EntryType& entry) {
  if (access.cursor == nullptr || access.mutex == nullptr || access.entries == nullptr) {
    return StatusCode::EngineInternalError;
  }
  const StatusCode lock_status = LockRingMutex(access.mutex);
  if (lock_status != StatusCode::Ok) {
    return lock_status;
  }
  if (access.cursor->size == access.cursor->capacity) {
    pthread_mutex_unlock(access.mutex);
    return StatusCode::QueueFull;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  entries[access.cursor->tail] = entry;
  access.cursor->tail = (access.cursor->tail + 1u) % access.cursor->capacity;
  ++access.cursor->size;
  pthread_mutex_unlock(access.mutex);
  return StatusCode::Ok;
}

template <typename EntryType>
bool PopRingEntry(Session::RingAccess access, EntryType* entry) {
  if (entry == nullptr || access.cursor == nullptr || access.mutex == nullptr ||
      access.entries == nullptr) {
    return false;
  }
  if (LockRingMutex(access.mutex) != StatusCode::Ok) {
    return false;
  }
  if (access.cursor->size == 0u) {
    pthread_mutex_unlock(access.mutex);
    return false;
  }
  auto* entries = static_cast<EntryType*>(access.entries);
  *entry = entries[access.cursor->head];
  access.cursor->head = (access.cursor->head + 1u) % access.cursor->capacity;
  --access.cursor->size;
  pthread_mutex_unlock(access.mutex);
  return true;
}

}  // namespace

Session::Session() = default;

Session::~Session() {
  Reset();
}

StatusCode Session::Attach(const BootstrapHandles& handles) {
  Reset();
  if (handles.shm_fd < 0) {
    return StatusCode::InvalidArgument;
  }

  LayoutConfig config;
  config.high_ring_size = 64;
  config.normal_ring_size = 256;
  config.response_ring_size = 256;
  config.slot_count = 128;

  // The creator writes the authoritative values into the header. Map enough
  // memory for a maximum-sized default first, then use the header values.
  Layout default_layout = ComputeLayout(config);
  mapped_region_ =
      mmap(nullptr, default_layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, handles.shm_fd, 0);
  if (mapped_region_ == MAP_FAILED) {
    mapped_region_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  header_ = static_cast<SharedMemoryHeader*>(mapped_region_);
  if (header_->magic != kSharedMemoryMagic || header_->protocol_version != kProtocolVersion) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }

  struct stat file_stat {};
  if (fstat(handles.shm_fd, &file_stat) != 0) {
    Reset();
    return StatusCode::EngineInternalError;
  }

  config.high_ring_size = header_->high_ring_size;
  config.normal_ring_size = header_->normal_ring_size;
  config.response_ring_size = header_->response_ring_size;
  config.slot_count = header_->slot_count;
  config.slot_size = header_->slot_size;
  if (!ValidateLayoutConfig(config, static_cast<std::size_t>(file_stat.st_size)) ||
      !ValidateRingCursor(header_->high_ring, config.high_ring_size) ||
      !ValidateRingCursor(header_->normal_ring, config.normal_ring_size) ||
      !ValidateRingCursor(header_->response_ring, config.response_ring_size)) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }
  Layout actual_layout = ComputeLayout(config);
  munmap(mapped_region_, default_layout.total_size);

  mapped_region_ =
      mmap(nullptr, actual_layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, handles.shm_fd, 0);
  if (mapped_region_ == MAP_FAILED) {
    mapped_region_ = nullptr;
    header_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  mapped_size_ = actual_layout.total_size;
  header_ = static_cast<SharedMemoryHeader*>(mapped_region_);
  handles_ = handles;
  return StatusCode::Ok;
}

void Session::Reset() {
  if (mapped_region_ != nullptr) {
    munmap(mapped_region_, mapped_size_);
  }
  if (handles_.shm_fd >= 0) {
    close(handles_.shm_fd);
  }
  if (handles_.high_req_event_fd >= 0) {
    close(handles_.high_req_event_fd);
  }
  if (handles_.normal_req_event_fd >= 0) {
    close(handles_.normal_req_event_fd);
  }
  if (handles_.resp_event_fd >= 0) {
    close(handles_.resp_event_fd);
  }
  mapped_size_ = 0;
  mapped_region_ = nullptr;
  header_ = nullptr;
  handles_ = {};
}

bool Session::valid() const {
  return header_ != nullptr;
}

const SharedMemoryHeader* Session::header() const {
  return header_;
}

SharedMemoryHeader* Session::mutable_header() {
  return header_;
}

const BootstrapHandles& Session::handles() const {
  return handles_;
}

SlotPayload* Session::slot_payload(uint32_t slot_index) {
  if (header_ == nullptr || slot_index >= header_->slot_count) {
    return nullptr;
  }
  LayoutConfig config{header_->high_ring_size,
                      header_->normal_ring_size,
                      header_->response_ring_size,
                      header_->slot_count,
                      header_->slot_size};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  return reinterpret_cast<SlotPayload*>(base + layout.slot_pool_offset +
                                        static_cast<std::size_t>(slot_index) * header_->slot_size);
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
  if (header_ == nullptr || mapped_region_ == nullptr) {
    return {};
  }
  LayoutConfig config{header_->high_ring_size,
                      header_->normal_ring_size,
                      header_->response_ring_size,
                      header_->slot_count,
                      header_->slot_size};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  switch (queue) {
    case QueueKind::HighRequest:
      return {&header_->high_ring, &header_->high_ring_mutex,
              base + layout.high_ring_offset};
    case QueueKind::NormalRequest:
      return {&header_->normal_ring, &header_->normal_ring_mutex,
              base + layout.normal_ring_offset};
    case QueueKind::Response:
      return {&header_->response_ring, &header_->response_ring_mutex,
              base + layout.response_ring_offset};
  }
  return {};
}

}  // namespace memrpc
