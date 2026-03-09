#include "memrpc/demo_bootstrap.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

#include "core/shm_layout.h"

namespace memrpc {

namespace {

uint64_t GenerateSessionId() {
  std::random_device device;
  std::mt19937_64 engine(device());
  return engine();
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

  ~Impl() {
    if (handles.shm_fd >= 0) {
      close(handles.shm_fd);
    }
    if (handles.high_req_event_fd >= 0) {
      close(handles.high_req_event_fd);
    }
    if (handles.normal_req_event_fd >= 0) {
      close(handles.normal_req_event_fd);
    }
    if (handles.resp_event_fd >= 0) {
      close(handles.resp_event_fd);
    }
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
    return StatusCode::kOk;
  }

  const int shm_fd =
      shm_open(impl_->config.shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (shm_fd < 0) {
    return StatusCode::kEngineInternalError;
  }

  const LayoutConfig layout_config{impl_->config.high_ring_size,
                                   impl_->config.normal_ring_size,
                                   impl_->config.response_ring_size,
                                   impl_->config.slot_count,
                                   sizeof(SlotPayload)};
  const Layout layout = ComputeLayout(layout_config);
  if (ftruncate(shm_fd, static_cast<off_t>(layout.total_size)) != 0) {
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::kEngineInternalError;
  }

  void* region =
      mmap(nullptr, layout.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (region == MAP_FAILED) {
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::kEngineInternalError;
  }
  std::memset(region, 0, layout.total_size);
  auto* header = static_cast<SharedMemoryHeader*>(region);
  header->magic = kSharedMemoryMagic;
  header->protocol_version = kProtocolVersion;
  header->session_id = GenerateSessionId();
  header->high_ring_size = impl_->config.high_ring_size;
  header->normal_ring_size = impl_->config.normal_ring_size;
  header->response_ring_size = impl_->config.response_ring_size;
  header->slot_count = impl_->config.slot_count;
  header->slot_size = sizeof(SlotPayload);
  header->high_ring.capacity = impl_->config.high_ring_size;
  header->normal_ring.capacity = impl_->config.normal_ring_size;
  header->response_ring.capacity = impl_->config.response_ring_size;
  const uint64_t session_id = header->session_id;
  if (!InitMutex(&header->high_ring_mutex) || !InitMutex(&header->normal_ring_mutex) ||
      !InitMutex(&header->response_ring_mutex)) {
    munmap(region, layout.total_size);
    close(shm_fd);
    shm_unlink(impl_->config.shm_name.c_str());
    return StatusCode::kEngineInternalError;
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
  return impl_->initialized ? StatusCode::kOk : StatusCode::kEngineInternalError;
}

StatusCode PosixDemoBootstrapChannel::Connect(BootstrapHandles* handles) {
  if (handles == nullptr || !impl_->initialized) {
    return StatusCode::kInvalidArgument;
  }

  handles->shm_fd = dup(impl_->handles.shm_fd);
  handles->high_req_event_fd = dup(impl_->handles.high_req_event_fd);
  handles->normal_req_event_fd = dup(impl_->handles.normal_req_event_fd);
  handles->resp_event_fd = dup(impl_->handles.resp_event_fd);
  handles->protocol_version = impl_->handles.protocol_version;
  handles->session_id = impl_->handles.session_id;
  return StatusCode::kOk;
}

StatusCode PosixDemoBootstrapChannel::NotifyPeerRestarted() {
  return StatusCode::kOk;
}

BootstrapHandles PosixDemoBootstrapChannel::server_handles() const {
  BootstrapHandles handles;
  handles.shm_fd = dup(impl_->handles.shm_fd);
  handles.high_req_event_fd = dup(impl_->handles.high_req_event_fd);
  handles.normal_req_event_fd = dup(impl_->handles.normal_req_event_fd);
  handles.resp_event_fd = dup(impl_->handles.resp_event_fd);
  handles.protocol_version = impl_->handles.protocol_version;
  handles.session_id = impl_->handles.session_id;
  return handles;
}

}  // namespace memrpc
