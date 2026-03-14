#include "memrpc/client/dev_bootstrap.h"

#include <array>
#include <cstddef>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iterator>
#include <random>

#include "core/shm_layout.h"
#include "virus_protection_service_log.h"

namespace MemRpc {

namespace {

void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

uint64_t GenerateSessionId() {
  std::random_device device;
  std::mt19937_64 engine(device());
  return engine();
}

bool DuplicateHandles(const BootstrapHandles& source, BootstrapHandles* target) {
  if (target == nullptr) {
    return false;
  }
  *target = {};

  using FdField = int BootstrapHandles::*;
  static constexpr std::array<FdField, 6> kFdFields{{
      &BootstrapHandles::shmFd,
      &BootstrapHandles::highReqEventFd,
      &BootstrapHandles::normalReqEventFd,
      &BootstrapHandles::respEventFd,
      &BootstrapHandles::reqCreditEventFd,
      &BootstrapHandles::respCreditEventFd,
  }};

  size_t dup_count = 0;
  for (FdField field : kFdFields) {
    target->*field = dup(source.*field);
    if (target->*field < 0) {
      for (size_t j = 0; j < dup_count; ++j) {
        CloseFd(&(target->*kFdFields[j]));
      }
      return false;
    }
    ++dup_count;
  }
  target->protocolVersion = source.protocolVersion;
  target->sessionId = source.sessionId;
  return true;
}

bool InitMutex(pthread_mutex_t* mutex) {
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

}  // namespace

struct DevBootstrapChannel::Impl {
  DevBootstrapConfig config;
  BootstrapHandles handles{};
  bool initialized = false;
  EngineDeathCallback death_callback;

  void ResetHandles() {
    CloseFd(&handles.shmFd);
    CloseFd(&handles.highReqEventFd);
    CloseFd(&handles.normalReqEventFd);
    CloseFd(&handles.respEventFd);
    CloseFd(&handles.reqCreditEventFd);
    CloseFd(&handles.respCreditEventFd);
    handles.protocolVersion = 0;
    handles.sessionId = 0;
    initialized = false;
  }

  [[nodiscard]] bool HasValidConfig() const {
    return config.maxRequestBytes != 0 &&
           config.maxResponseBytes != 0 &&
           config.maxResponseBytes <= DEFAULT_MAX_RESPONSE_BYTES;
  }

  [[nodiscard]] Layout BuildLayout() const {
    const LayoutConfig layout_config{config.highRingSize,
                                     config.normalRingSize,
                                     config.responseRingSize,
                                     config.slotCount,
                                     ComputeSlotSize(config.maxRequestBytes,
                                                     config.maxResponseBytes),
                                     config.maxRequestBytes,
                                     config.maxResponseBytes};
    return ComputeLayout(layout_config);
  }

  void PopulateHeader(SharedMemoryHeader* header) const {
    header->magic = SHARED_MEMORY_MAGIC;
    header->protocolVersion = PROTOCOL_VERSION;
    header->sessionId = GenerateSessionId();
    header->sessionState = 0;
    header->highRingSize = config.highRingSize;
    header->normalRingSize = config.normalRingSize;
    header->responseRingSize = config.responseRingSize;
    header->slotCount = config.slotCount;
    header->slotSize = ComputeSlotSize(config.maxRequestBytes, config.maxResponseBytes);
    header->maxRequestBytes = config.maxRequestBytes;
    header->maxResponseBytes = config.maxResponseBytes;
    header->highRing.capacity = config.highRingSize;
    header->normalRing.capacity = config.normalRingSize;
    header->responseRing.capacity = config.responseRingSize;
  }

  static void* ResponseSlotPoolRegion(void* region, const Layout& layout) {
    auto* bytes = static_cast<uint8_t*>(region);
    return std::next(bytes, static_cast<std::ptrdiff_t>(layout.responseSlotPoolOffset));
  }

  StatusCode InitializeSharedMemory(int shmFd, uint64_t* out_session_id) const {
    const Layout layout = BuildLayout();
    if (ftruncate(shmFd, static_cast<off_t>(layout.totalSize)) != 0) {
      HILOGE("ftruncate failed, size=%{public}zu errno=%{public}d", layout.totalSize, errno);
      return StatusCode::EngineInternalError;
    }

    void* region =
        mmap(nullptr, layout.totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (region == MAP_FAILED) {
      HILOGE("mmap failed, size=%{public}zu errno=%{public}d", layout.totalSize, errno);
      return StatusCode::EngineInternalError;
    }
    std::memset(region, 0, layout.totalSize);
    auto* header = static_cast<SharedMemoryHeader*>(region);
    PopulateHeader(header);
    *out_session_id = header->sessionId;
    if (!InitMutex(&header->clientStateMutex) ||
        !InitializeSharedSlotPool(ResponseSlotPoolRegion(region, layout),
                                  config.responseRingSize)) {
      HILOGE("InitMutex failed");
      munmap(region, layout.totalSize);
      return StatusCode::EngineInternalError;
    }
    munmap(region, layout.totalSize);
    return StatusCode::Ok;
  }

  bool CreateEventFds() {
    handles.highReqEventFd = eventfd(0, EFD_NONBLOCK);
    handles.normalReqEventFd = eventfd(0, EFD_NONBLOCK);
    handles.respEventFd = eventfd(0, EFD_NONBLOCK);
    handles.reqCreditEventFd = eventfd(0, EFD_NONBLOCK);
    handles.respCreditEventFd = eventfd(0, EFD_NONBLOCK);
    return handles.highReqEventFd >= 0 &&
           handles.normalReqEventFd >= 0 &&
           handles.respEventFd >= 0 &&
           handles.reqCreditEventFd >= 0 &&
           handles.respCreditEventFd >= 0;
  }

  [[nodiscard]] int OpenSharedMemoryFd() const {
    const int shm_fd =
        shm_open(config.shmName.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (shm_fd < 0) {
      HILOGE("shm_open failed, name=%{public}s errno=%{public}d", config.shmName.c_str(),
             errno);
    }
    return shm_fd;
  }

  StatusCode FinalizeInitializedSession(int shmFd) {
    uint64_t session_id = 0;
    const StatusCode init_status = InitializeSharedMemory(shmFd, &session_id);
    if (init_status != StatusCode::Ok) {
      close(shmFd);
      shm_unlink(config.shmName.c_str());
      return init_status;
    }

    handles.shmFd = shmFd;
    handles.protocolVersion = PROTOCOL_VERSION;
    handles.sessionId = session_id;
    initialized = CreateEventFds();
    if (initialized) {
      return StatusCode::Ok;
    }

    HILOGE("eventfd initialization failed");
    ResetHandles();
    shm_unlink(config.shmName.c_str());
    return StatusCode::EngineInternalError;
  }

  StatusCode EnsureInitialized() {
    if (initialized) {
      return StatusCode::Ok;
    }
    if (!HasValidConfig()) {
      HILOGE("invalid bootstrap config, request=%{public}u response=%{public}u",
             config.maxRequestBytes, config.maxResponseBytes);
      return StatusCode::InvalidArgument;
    }

    ResetHandles();
    const int shm_fd = OpenSharedMemoryFd();
    if (shm_fd < 0) {
      return StatusCode::EngineInternalError;
    }
    return FinalizeInitializedSession(shm_fd);
  }

  ~Impl() {
    ResetHandles();
    if (!config.shmName.empty()) {
      shm_unlink(config.shmName.c_str());
    }
  }
};

DevBootstrapChannel::DevBootstrapChannel(DevBootstrapConfig config)
    : impl_(std::make_shared<Impl>()) {
  impl_->config = std::move(config);
  if (impl_->config.shmName.empty()) {
    impl_->config.shmName = "/MemRpc-dev-" + std::to_string(::getpid());
  }
}

DevBootstrapChannel::~DevBootstrapChannel() = default;

StatusCode DevBootstrapChannel::OpenSession(BootstrapHandles& handles) {
  const StatusCode init_status = impl_->EnsureInitialized();
  if (init_status != StatusCode::Ok) {
    return init_status;
  }

  if (!DuplicateHandles(impl_->handles, &handles)) {
    HILOGE("OpenSession failed while duplicating bootstrap handles");
    return StatusCode::EngineInternalError;
  }
  return StatusCode::Ok;
}

StatusCode DevBootstrapChannel::CloseSession() {
  return StatusCode::Ok;
}

ChannelHealthResult DevBootstrapChannel::CheckHealth(uint64_t expectedSessionId) {
  (void)expectedSessionId;
  return {};
}

void DevBootstrapChannel::SetEngineDeathCallback(EngineDeathCallback callback) {
  impl_->death_callback = std::move(callback);
}

BootstrapHandles DevBootstrapChannel::serverHandles() const {
  BootstrapHandles handles;
  if (!DuplicateHandles(impl_->handles, &handles)) {
    HILOGE("server_handles failed while duplicating bootstrap handles");
  }
  return handles;
}

void DevBootstrapChannel::SimulateEngineDeathForTest(uint64_t session_id) {
  const uint64_t dead_session_id =
      session_id == 0 ? impl_->handles.sessionId : session_id;
  if (session_id == 0 || session_id == impl_->handles.sessionId) {
    impl_->ResetHandles();
    if (!impl_->config.shmName.empty()) {
      shm_unlink(impl_->config.shmName.c_str());
    }
  }
  if (impl_->death_callback) {
    impl_->death_callback(dead_session_id);
  }
}

}  // namespace MemRpc
