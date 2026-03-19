#include "memrpc/client/sa_bootstrap.h"

#include <utility>

#include "memrpc/client/dev_bootstrap.h"
#include "virus_protection_service_log.h"

namespace MemRpc {

struct SaBootstrapChannel::Impl {
  explicit Impl(SaBootstrapConfig value)
      : config(std::move(value)),
        fallback(std::make_shared<DevBootstrapChannel>()) {}

  SaBootstrapConfig config;
  std::shared_ptr<DevBootstrapChannel> fallback;
  std::string lastError;
};

SaBootstrapChannel::SaBootstrapChannel(SaBootstrapConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SaBootstrapChannel::~SaBootstrapChannel() = default;

const SaBootstrapConfig& SaBootstrapChannel::Config() const {
  return impl_->config;
}

const std::string& SaBootstrapChannel::LastError() const {
  return impl_->lastError;
}

BootstrapHandles SaBootstrapChannel::ServerHandles() const {
  if (impl_->fallback == nullptr) {
    HILOGE("SaBootstrapChannel::ServerHandles failed: fallback is null");
    return {};
  }
  return impl_->fallback->serverHandles();
}

void SaBootstrapChannel::SimulateEngineDeathForTest(uint64_t session_id) {
  if (impl_->fallback != nullptr) {
    impl_->fallback->SimulateEngineDeathForTest(session_id);
  }
}

StatusCode SaBootstrapChannel::OpenSession(BootstrapHandles& handles) {
  if (impl_->fallback == nullptr) {
    handles = BootstrapHandles{};
    impl_->lastError = "Fake SA bootstrap has no dev fallback channel.";
    HILOGE("SaBootstrapChannel::OpenSession failed: fallback is null");
    return StatusCode::EngineInternalError;
  }
  const StatusCode status = impl_->fallback->OpenSession(handles);
  if (status != StatusCode::Ok) {
    impl_->lastError = "Fake SA bootstrap OpenSession failed on dev fallback.";
    HILOGE("SaBootstrapChannel::OpenSession failed: status=%{public}d", static_cast<int>(status));
  } else {
    impl_->lastError.clear();
  }
  return status;
}

StatusCode SaBootstrapChannel::CloseSession() {
  if (impl_->fallback == nullptr) {
    impl_->lastError = "Fake SA bootstrap has no dev fallback channel.";
    HILOGE("SaBootstrapChannel::CloseSession failed: fallback is null");
    return StatusCode::EngineInternalError;
  }
  const StatusCode status = impl_->fallback->CloseSession();
  if (status != StatusCode::Ok) {
    HILOGE("SaBootstrapChannel::CloseSession failed: status=%{public}d", static_cast<int>(status));
  }
  return status;
}

ChannelHealthResult SaBootstrapChannel::CheckHealth(uint64_t expectedSessionId) {
  if (impl_->fallback == nullptr) {
    impl_->lastError = "Fake SA bootstrap has no dev fallback channel.";
    HILOGE("SaBootstrapChannel::CheckHealth failed: fallback is null expected_session=%{public}llu",
           static_cast<unsigned long long>(expectedSessionId));
    return {};
  }
  return impl_->fallback->CheckHealth(expectedSessionId);
}

void SaBootstrapChannel::SetEngineDeathCallback(EngineDeathCallback callback) {
  if (impl_->fallback != nullptr) {
    impl_->fallback->SetEngineDeathCallback(std::move(callback));
  }
}

}  // namespace MemRpc
