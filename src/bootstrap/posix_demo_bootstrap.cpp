#include "memrpc/client/demo_bootstrap.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

#include "core/shm_layout.h"
#include "virus_protection_service_log.h"

namespace memrpc {

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

int DuplicateFdWithTestHook(int fd, int* remaining_successes_before_failure) {
  if (remaining_successes_before_failure != nullptr && *remaining_successes_before_failure == 0) {
    errno = EMFILE;
    return -1;
  }
  if (remaining_successes_before_failure != nullptr && *remaining_successes_before_failure > 0) {
    --(*remaining_successes_before_failure);
  }
  return dup(fd);
}

bool DuplicateHandles(const BootstrapHandles& source, BootstrapHandles* target,
                      int* remaining_successes_before_failure) {
  if (target == nullptr) {
    return false;
  }
  *target = {};

  using FdField = int BootstrapHandles::*;
  static constexpr FdField kFdFields[] = {
      &BootstrapHandles::shm_fd,
      &BootstrapHandles::high_req_event_fd,
      &BootstrapHandles::normal_req_event_fd,
      &BootstrapHandles::resp_event_fd,
      &BootstrapHandles::req_credit_event_fd,
      &BootstrapHandles::resp_credit_event_fd,
  };

  size_t dup_count = 0;
  for (FdField field : kFdFields) {
    target->*field = DuplicateFdWithTestHook(source.*field, remaining_successes_before_failure);
    if (target->*field < 0) {
      for (size_t j = 0; j < dup_count; ++j) {
        CloseFd(&(target->*kFdFields[j]));
      }
      return false;
    }
    ++dup_count;
  }
  target->protocol_version = source.protocol_version;
  target->session_id = source.session_id;
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

struct PosixDemoBootstrapChannel::Impl {
  DemoBootstrapConfig config;
  BootstrapHandles handles{};
  bool initialized = false;
  EngineDeathCallback death_callback;
  int dup_fail_after_count = -1;

  void ResetHandles() {
    CloseFd(&handles.shm_fd);
    CloseFd(&handles.high_req_event_fd);
    CloseFd(&handles.normal_req_event_fd);
    CloseFd(&handles.resp_event_fd);
    CloseFd(&handles.req_credit_event_fd);
    CloseFd(&handles.resp_credit_event_fd);
    handles.protocol_version = 0;
    handles.session_id = 0;
    initialized = false;
  }

  StatusCode InitializeSharedMemory(int shm_fd, uint64_t* out_session_id) {
    const LayoutConfig layout_config{config.high_ring_size,
                                     config.normal_ring_size,
                                     config.response_ring_size,
                                     config.slot_count,
                                     ComputeSlotSize(config.max_request_bytes,
                                                     config.max_response_bytes),
                                     config.max_request_bytes,
                                     config.max_response_bytes};
    const Layout layout = ComputeLayout(layout_config);
    if (ftruncate(shm_fd, static_cast<off_t>(layout.total_size)) != 0) {
      HLOGE("ftruncate failed, size=%{public}zu errno=%{public}d", layout.total_size, errno);
      return StatusCode::EngineInternalError;
    }

    void* region =
        mmap(nullptr, layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED) {
      HLOGE("mmap failed, size=%{public}zu errno=%{public}d", layout.total_size, errno);
      return StatusCode::EngineInternalError;
    }
    std::memset(region, 0, layout.total_size);
    auto* header = static_cast<SharedMemoryHeader*>(region);
    header->magic = kSharedMemoryMagic;
    header->protocol_version = kProtocolVersion;
    header->session_id = GenerateSessionId();
    header->session_state = 0;
    header->high_ring_size = config.high_ring_size;
    header->normal_ring_size = config.normal_ring_size;
    header->response_ring_size = config.response_ring_size;
    header->slot_count = config.slot_count;
    header->slot_size = ComputeSlotSize(config.max_request_bytes, config.max_response_bytes);
    header->max_request_bytes = config.max_request_bytes;
    header->max_response_bytes = config.max_response_bytes;
    header->high_ring.capacity = config.high_ring_size;
    header->normal_ring.capacity = config.normal_ring_size;
    header->response_ring.capacity = config.response_ring_size;
    *out_session_id = header->session_id;
    if (!InitMutex(&header->client_state_mutex) ||
        !InitializeSharedSlotPool(static_cast<uint8_t*>(region) + layout.response_slot_pool_offset,
                                  config.response_ring_size)) {
      HLOGE("InitMutex failed");
      munmap(region, layout.total_size);
      return StatusCode::EngineInternalError;
    }
    munmap(region, layout.total_size);
    return StatusCode::Ok;
  }

  bool CreateEventFds() {
    handles.high_req_event_fd = eventfd(0, EFD_NONBLOCK);
    handles.normal_req_event_fd = eventfd(0, EFD_NONBLOCK);
    handles.resp_event_fd = eventfd(0, EFD_NONBLOCK);
    handles.req_credit_event_fd = eventfd(0, EFD_NONBLOCK);
    handles.resp_credit_event_fd = eventfd(0, EFD_NONBLOCK);
    return handles.high_req_event_fd >= 0 &&
           handles.normal_req_event_fd >= 0 &&
           handles.resp_event_fd >= 0 &&
           handles.req_credit_event_fd >= 0 &&
           handles.resp_credit_event_fd >= 0;
  }

  ~Impl() {
    ResetHandles();
    if (!config.shm_name.empty()) {
      shm_unlink(config.shm_name.c_str());
    }
  }
};

PosixDemoBootstrapChannel::PosixDemoBootstrapChannel(DemoBootstrapConfig config)
    : impl_(std::make_shared<Impl>()) {
  impl_->config = std::move(config);
  if (impl_->config.shm_name.empty()) {
    impl_->config.shm_name = "/memrpc-demo-" + std::to_string(::getpid());
  }
}

PosixDemoBootstrapChannel::~PosixDemoBootstrapChannel() = default;

StatusCode PosixDemoBootstrapChannel::OpenSession(BootstrapHandles* handles) {
  if (handles == nullptr) {
    return StatusCode::InvalidArgument;
  }

  if (!impl_->initialized) {
    if (impl_->config.max_request_bytes == 0 ||
        impl_->config.max_response_bytes == 0 ||
        impl_->config.max_response_bytes > kDefaultMaxResponseBytes) {
      HLOGE("invalid bootstrap config, request=%{public}u response=%{public}u",
            impl_->config.max_request_bytes, impl_->config.max_response_bytes);
      return StatusCode::InvalidArgument;
    }

    impl_->ResetHandles();

    const int shm_fd =
        shm_open(impl_->config.shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (shm_fd < 0) {
      HLOGE("shm_open failed, name=%{public}s errno=%{public}d", impl_->config.shm_name.c_str(),
            errno);
      return StatusCode::EngineInternalError;
    }

    uint64_t session_id = 0;
    const StatusCode init_status = impl_->InitializeSharedMemory(shm_fd, &session_id);
    if (init_status != StatusCode::Ok) {
      close(shm_fd);
      shm_unlink(impl_->config.shm_name.c_str());
      return init_status;
    }

    impl_->handles.shm_fd = shm_fd;
    impl_->handles.protocol_version = kProtocolVersion;
    impl_->handles.session_id = session_id;
    impl_->initialized = impl_->CreateEventFds();
    if (!impl_->initialized) {
      HLOGE("eventfd initialization failed");
      impl_->ResetHandles();
      shm_unlink(impl_->config.shm_name.c_str());
      return StatusCode::EngineInternalError;
    }
  }

  if (!DuplicateHandles(impl_->handles, handles, &impl_->dup_fail_after_count)) {
    HLOGE("OpenSession failed while duplicating bootstrap handles");
    return StatusCode::EngineInternalError;
  }
  return StatusCode::Ok;
}

StatusCode PosixDemoBootstrapChannel::CloseSession() {
  return StatusCode::Ok;
}

void PosixDemoBootstrapChannel::SetEngineDeathCallback(EngineDeathCallback callback) {
  impl_->death_callback = std::move(callback);
}

BootstrapHandles PosixDemoBootstrapChannel::server_handles() const {
  BootstrapHandles handles;
  if (!DuplicateHandles(impl_->handles, &handles, &impl_->dup_fail_after_count)) {
    HLOGE("server_handles failed while duplicating bootstrap handles");
  }
  return handles;
}

void PosixDemoBootstrapChannel::SimulateEngineDeathForTest(uint64_t session_id) {
  const uint64_t dead_session_id =
      session_id == 0 ? impl_->handles.session_id : session_id;
  if (session_id == 0 || session_id == impl_->handles.session_id) {
    impl_->ResetHandles();
    if (!impl_->config.shm_name.empty()) {
      shm_unlink(impl_->config.shm_name.c_str());
    }
  }
  if (impl_->death_callback) {
    impl_->death_callback(dead_session_id);
  }
}

void PosixDemoBootstrapChannel::SetDupFailureAfterCountForTest(int count) {
  impl_->dup_fail_after_count = count;
}

}  // namespace memrpc
