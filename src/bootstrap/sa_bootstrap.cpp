#include "memrpc/client/sa_bootstrap.h"

#include <utility>

#include "memrpc/client/demo_bootstrap.h"

namespace memrpc {

struct SaBootstrapChannel::Impl {
  explicit Impl(SaBootstrapConfig value)
      : config(std::move(value)),
        fallback(std::make_shared<PosixDemoBootstrapChannel>()) {}

  SaBootstrapConfig config;
  std::shared_ptr<PosixDemoBootstrapChannel> fallback;
  std::string last_error;
};

SaBootstrapChannel::SaBootstrapChannel(SaBootstrapConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SaBootstrapChannel::~SaBootstrapChannel() = default;

const SaBootstrapConfig& SaBootstrapChannel::config() const {
  return impl_->config;
}

const std::string& SaBootstrapChannel::last_error() const {
  return impl_->last_error;
}

BootstrapHandles SaBootstrapChannel::server_handles() const {
  if (impl_->fallback == nullptr) {
    return {};
  }
  return impl_->fallback->serverHandles();
}

void SaBootstrapChannel::SimulateEngineDeathForTest(uint64_t session_id) {
  if (impl_->fallback != nullptr) {
    impl_->fallback->SimulateEngineDeathForTest(session_id);
  }
}

StatusCode SaBootstrapChannel::OpenSession(BootstrapHandles* handles) {
  if (handles == nullptr) {
    impl_->last_error = "Fake SA bootstrap received a null BootstrapHandles.";
    return StatusCode::InvalidArgument;
  }
  if (impl_->fallback == nullptr) {
    *handles = BootstrapHandles{};
    impl_->last_error = "Fake SA bootstrap has no POSIX fallback channel.";
    return StatusCode::EngineInternalError;
  }
  const StatusCode status = impl_->fallback->OpenSession(handles);
  if (status != StatusCode::Ok) {
    impl_->last_error = "Fake SA bootstrap OpenSession failed on POSIX fallback.";
  } else {
    impl_->last_error.clear();
  }
  return status;
}

StatusCode SaBootstrapChannel::CloseSession() {
  if (impl_->fallback == nullptr) {
    impl_->last_error = "Fake SA bootstrap has no POSIX fallback channel.";
    return StatusCode::EngineInternalError;
  }
  return impl_->fallback->CloseSession();
}

void SaBootstrapChannel::SetEngineDeathCallback(EngineDeathCallback callback) {
  if (impl_->fallback != nullptr) {
    impl_->fallback->SetEngineDeathCallback(std::move(callback));
  }
}

}  // namespace memrpc
