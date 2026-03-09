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
  target->shm_fd = DuplicateFdWithTestHook(source.shm_fd, remaining_successes_before_failure);
  if (target->shm_fd < 0) {
    return false;
  }
  target->high_req_event_fd =
      DuplicateFdWithTestHook(source.high_req_event_fd, remaining_successes_before_failure);
  if (target->high_req_event_fd < 0) {
    CloseFd(&target->shm_fd);
    return false;
  }
  target->normal_req_event_fd =
      DuplicateFdWithTestHook(source.normal_req_event_fd, remaining_successes_before_failure);
  if (target->normal_req_event_fd < 0) {
    CloseFd(&target->shm_fd);
    CloseFd(&target->high_req_event_fd);
    return false;
  }
  target->resp_event_fd =
      DuplicateFdWithTestHook(source.resp_event_fd, remaining_successes_before_failure);
  if (target->resp_event_fd < 0) {
    CloseFd(&target->shm_fd);
    CloseFd(&target->high_req_event_fd);
    CloseFd(&target->normal_req_event_fd);
    return false;
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
    handles.protocol_version = 0;
    handles.session_id = 0;
    initialized = false;
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

StatusCode PosixDemoBootstrapChannel::StartEngine() {
  if (impl_->initialized) {
    return StatusCode::Ok;
  }

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

  const LayoutConfig layout_config{impl_->config.high_ring_size,
                                   impl_->config.normal_ring_size,
                                   impl_->config.response_ring_size,
                                   impl_->config.slot_count,
                                   ComputeSlotSize(impl_->config.max_request_bytes,
                                                   impl_->config.max_response_bytes),
                                   impl_->config.max_request_bytes,
                                   impl_->config.max_response_bytes};
  const Layout layout = ComputeLayout(layout_config);
  if (ftruncate(shm_fd, static_cast<off_t>(layout.total_size)) != 0) {
    HLOGE("ftruncate failed, size=%{public}zu errno=%{public}d", layout.total_size, errno);
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::EngineInternalError;
  }

  void* region =
      mmap(nullptr, layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (region == MAP_FAILED) {
    HLOGE("mmap failed, size=%{public}zu errno=%{public}d", layout.total_size, errno);
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::EngineInternalError;
  }
  std::memset(region, 0, layout.total_size);
  auto* header = static_cast<SharedMemoryHeader*>(region);
  header->magic = kSharedMemoryMagic;
  header->protocol_version = kProtocolVersion;
  header->session_id = GenerateSessionId();
  header->session_state = 0;
  header->high_ring_size = impl_->config.high_ring_size;
  header->normal_ring_size = impl_->config.normal_ring_size;
  header->response_ring_size = impl_->config.response_ring_size;
  header->slot_count = impl_->config.slot_count;
  header->slot_size =
      ComputeSlotSize(impl_->config.max_request_bytes, impl_->config.max_response_bytes);
  header->max_request_bytes = impl_->config.max_request_bytes;
  header->max_response_bytes = impl_->config.max_response_bytes;
  header->high_ring.capacity = impl_->config.high_ring_size;
  header->normal_ring.capacity = impl_->config.normal_ring_size;
  header->response_ring.capacity = impl_->config.response_ring_size;
  const uint64_t session_id = header->session_id;
  if (!InitMutex(&header->high_ring_mutex) || !InitMutex(&header->normal_ring_mutex) ||
      !InitMutex(&header->response_ring_mutex)) {
    HLOGE("InitMutex failed");
    munmap(region, layout.total_size);
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::EngineInternalError;
  }
  munmap(region, layout.total_size);

  impl_->handles.shm_fd = shm_fd;
  impl_->handles.high_req_event_fd = eventfd(0, EFD_NONBLOCK);
  impl_->handles.normal_req_event_fd = eventfd(0, EFD_NONBLOCK);
  impl_->handles.resp_event_fd = eventfd(0, EFD_NONBLOCK);
  impl_->handles.protocol_version = kProtocolVersion;
  impl_->handles.session_id = session_id;
  impl_->initialized = impl_->handles.high_req_event_fd >= 0 &&
                       impl_->handles.normal_req_event_fd >= 0 &&
                       impl_->handles.resp_event_fd >= 0;
  if (!impl_->initialized) {
    HLOGE("eventfd initialization failed");
    impl_->ResetHandles();
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::EngineInternalError;
  }
  return StatusCode::Ok;
}

StatusCode PosixDemoBootstrapChannel::Connect(BootstrapHandles* handles) {
  if (handles == nullptr || !impl_->initialized) {
    return StatusCode::InvalidArgument;
  }
  if (!DuplicateHandles(impl_->handles, handles, &impl_->dup_fail_after_count)) {
    HLOGE("Connect failed while duplicating bootstrap handles");
    return StatusCode::EngineInternalError;
  }
  return StatusCode::Ok;
}

StatusCode PosixDemoBootstrapChannel::NotifyPeerRestarted() {
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
