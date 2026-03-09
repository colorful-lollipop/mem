#include "memrpc/sa_bootstrap.h"

#include <utility>

#include "memrpc/demo_bootstrap.h"

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
  return impl_->fallback->server_handles();
}

StatusCode SaBootstrapChannel::StartEngine() {
  if (impl_->fallback == nullptr) {
    impl_->last_error = "Fake SA bootstrap has no POSIX fallback channel.";
    return StatusCode::kEngineInternalError;
  }
  const StatusCode status = impl_->fallback->StartEngine();
  if (status != StatusCode::kOk) {
    impl_->last_error = "Fake SA bootstrap failed to initialize POSIX transport.";
  } else {
    impl_->last_error.clear();
  }
  return status;
}

StatusCode SaBootstrapChannel::Connect(BootstrapHandles* handles) {
  if (handles == nullptr) {
    impl_->last_error = "Fake SA bootstrap received a null BootstrapHandles.";
    return StatusCode::kInvalidArgument;
  }
  if (impl_->fallback == nullptr) {
    *handles = BootstrapHandles{};
    impl_->last_error = "Fake SA bootstrap has no POSIX fallback channel.";
    return StatusCode::kEngineInternalError;
  }
  const StatusCode status = impl_->fallback->Connect(handles);
  if (status != StatusCode::kOk) {
    impl_->last_error =
        "Fake SA bootstrap could not connect. Call StartEngine first or "
        "provide a platform-specific adapter.";
  } else {
    impl_->last_error.clear();
  }
  return status;
}

StatusCode SaBootstrapChannel::NotifyPeerRestarted() {
  if (impl_->fallback == nullptr) {
    impl_->last_error = "Fake SA bootstrap has no POSIX fallback channel.";
    return StatusCode::kEngineInternalError;
  }
  return impl_->fallback->NotifyPeerRestarted();
}

}  // namespace memrpc
