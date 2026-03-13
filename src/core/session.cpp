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
  if (config.high_ring_size == 0 || config.high_ring_size > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.normal_ring_size == 0 || config.normal_ring_size > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.response_ring_size == 0 || config.response_ring_size > MAX_RING_ENTRIES) {
    return false;
  }
  if (config.slot_count == 0 || config.slot_count > MAX_SLOT_COUNT) {
    return false;
  }
  if (config.max_request_bytes == 0 || config.max_response_bytes == 0 ||
      config.max_response_bytes > DEFAULT_MAX_RESPONSE_BYTES) {
    return false;
  }
  if (config.slot_size != ComputeSlotSize(config.max_request_bytes, config.max_response_bytes)) {
    return false;
  }

  const Layout layout = ComputeLayout(config);
  if (layout.total_size < sizeof(SharedMemoryHeader) || layout.total_size > file_size) {
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

StatusCode Session::MapAndValidateHeader(int shm_fd) {
  LayoutConfig config;
  config.high_ring_size = 32;
  config.normal_ring_size = 32;
  config.response_ring_size = 64;
  config.slot_count = 64;
  config.slot_size = ComputeSlotSize(DEFAULT_MAX_REQUEST_BYTES, DEFAULT_MAX_RESPONSE_BYTES);
  config.max_request_bytes = DEFAULT_MAX_REQUEST_BYTES;
  config.max_response_bytes = DEFAULT_MAX_RESPONSE_BYTES;

  Layout default_layout = ComputeLayout(config);
  initial_mapped_size_ = default_layout.total_size;
  mapped_region_ =
      mmap(nullptr, default_layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (mapped_region_ == MAP_FAILED) {
    mapped_region_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  header_ = static_cast<SharedMemoryHeader*>(mapped_region_);
  if (header_->magic != SHARED_MEMORY_MAGIC || header_->protocol_version != PROTOCOL_VERSION) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }
  return StatusCode::Ok;
}

StatusCode Session::RemapWithActualLayout(int shm_fd) {
  struct stat file_stat {};
  if (fstat(shm_fd, &file_stat) != 0) {
    Reset();
    return StatusCode::EngineInternalError;
  }

  LayoutConfig config;
  config.high_ring_size = header_->high_ring_size;
  config.normal_ring_size = header_->normal_ring_size;
  config.response_ring_size = header_->response_ring_size;
  config.slot_count = header_->slot_count;
  config.slot_size = header_->slot_size;
  config.max_request_bytes = header_->max_request_bytes;
  config.max_response_bytes = header_->max_response_bytes;
  if (!ValidateLayoutConfig(config, static_cast<std::size_t>(file_stat.st_size)) ||
      !ValidateRingCursor(header_->high_ring, config.high_ring_size) ||
      !ValidateRingCursor(header_->normal_ring, config.normal_ring_size) ||
      !ValidateRingCursor(header_->response_ring, config.response_ring_size)) {
    Reset();
    return StatusCode::ProtocolMismatch;
  }
  Layout actual_layout = ComputeLayout(config);
  munmap(mapped_region_, initial_mapped_size_);

  mapped_region_ =
      mmap(nullptr, actual_layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (mapped_region_ == MAP_FAILED) {
    mapped_region_ = nullptr;
    header_ = nullptr;
    return StatusCode::EngineInternalError;
  }

  mapped_size_ = actual_layout.total_size;
  header_ = static_cast<SharedMemoryHeader*>(mapped_region_);
  return StatusCode::Ok;
}

StatusCode Session::TryAcquireClientSlot() {
  const StatusCode lock_status = LockSharedMutex(&header_->client_state_mutex);
  if (lock_status != StatusCode::Ok) {
    Reset();
    return lock_status;
  }
  if (header_->client_attached != 0) {
    if (!ProcessIsAlive(header_->active_client_pid)) {
      header_->client_attached = 0;
      header_->active_client_pid = 0;
    } else {
      pthread_mutex_unlock(&header_->client_state_mutex);
      Reset();
      return StatusCode::InvalidArgument;
    }
  }
  header_->client_attached = 1;
  header_->active_client_pid = static_cast<uint32_t>(getpid());
  owns_client_slot_ = true;
  pthread_mutex_unlock(&header_->client_state_mutex);
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
  attach_role_ = role;
  owns_client_slot_ = false;
  if (role == AttachRole::Client) {
    status = TryAcquireClientSlot();
    if (status != StatusCode::Ok) {
      return status;
    }
  }
  return StatusCode::Ok;
}

void Session::Reset() {
  if (header_ != nullptr && owns_client_slot_) {
    if (LockSharedMutex(&header_->client_state_mutex) == StatusCode::Ok) {
      header_->client_attached = 0;
      header_->active_client_pid = 0;
      pthread_mutex_unlock(&header_->client_state_mutex);
    }
  }
  if (mapped_region_ != nullptr) {
    munmap(mapped_region_, mapped_size_);
  }
  if (handles_.shmFd >= 0) {
    close(handles_.shmFd);
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
  if (handles_.req_credit_event_fd >= 0) {
    close(handles_.req_credit_event_fd);
  }
  if (handles_.resp_credit_event_fd >= 0) {
    close(handles_.resp_credit_event_fd);
  }
  mapped_size_ = 0;
  mapped_region_ = nullptr;
  header_ = nullptr;
  handles_ = {};
  attach_role_ = AttachRole::Server;
  owns_client_slot_ = false;
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

Session::SessionState Session::state() const {
  if (header_ == nullptr) {
    return SessionState::Broken;
  }
  return static_cast<SessionState>(header_->session_state);
}

void Session::SetState(SessionState state) {
  if (header_ != nullptr) {
    header_->session_state = static_cast<uint32_t>(state);
  }
}

SlotPayload* Session::slot_payload(uint32_t slot_index) {
  if (header_ == nullptr || slot_index >= header_->slot_count) {
    return nullptr;
  }
  LayoutConfig config{header_->high_ring_size,
                      header_->normal_ring_size,
                      header_->response_ring_size,
                      header_->slot_count,
                      header_->slot_size,
                      header_->max_request_bytes,
                      header_->max_response_bytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  return reinterpret_cast<SlotPayload*>(base + layout.slot_pool_offset +
                                        static_cast<std::size_t>(slot_index) * header_->slot_size);
}

uint8_t* Session::slot_request_bytes(uint32_t slot_index) {
  SlotPayload* payload = slot_payload(slot_index);
  if (payload == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<uint8_t*>(payload);
  return base + sizeof(SlotPayload);
}

ResponseSlotPayload* Session::response_slot_payload(uint32_t slot_index) {
  if (header_ == nullptr || slot_index >= header_->response_ring_size) {
    return nullptr;
  }
  LayoutConfig config{header_->high_ring_size,
                      header_->normal_ring_size,
                      header_->response_ring_size,
                      header_->slot_count,
                      header_->slot_size,
                      header_->max_request_bytes,
                      header_->max_response_bytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  return reinterpret_cast<ResponseSlotPayload*>(
      base + layout.response_slots_offset +
      static_cast<std::size_t>(slot_index) * ComputeResponseSlotSize(header_->max_response_bytes));
}

uint8_t* Session::response_slot_bytes(uint32_t slot_index) {
  ResponseSlotPayload* payload = response_slot_payload(slot_index);
  if (payload == nullptr) {
    return nullptr;
  }
  auto* base = reinterpret_cast<uint8_t*>(payload);
  return base + sizeof(ResponseSlotPayload);
}

void* Session::response_slot_pool_region() {
  if (header_ == nullptr || mapped_region_ == nullptr) {
    return nullptr;
  }
  LayoutConfig config{header_->high_ring_size,
                      header_->normal_ring_size,
                      header_->response_ring_size,
                      header_->slot_count,
                      header_->slot_size,
                      header_->max_request_bytes,
                      header_->max_response_bytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  return base + layout.response_slot_pool_offset;
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
                      header_->slot_size,
                      header_->max_request_bytes,
                      header_->max_response_bytes};
  Layout layout = ComputeLayout(config);
  auto* base = static_cast<std::byte*>(mapped_region_);
  switch (queue) {
    case QueueKind::HighRequest:
      return {&header_->high_ring, base + layout.high_ring_offset};
    case QueueKind::NormalRequest:
      return {&header_->normal_ring, base + layout.normal_ring_offset};
    case QueueKind::Response:
      return {&header_->response_ring, base + layout.response_ring_offset};
  }
  return {};
}

}  // namespace memrpc
