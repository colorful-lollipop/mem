#ifndef MEMRPC_SA_BOOTSTRAP_H_
#define MEMRPC_SA_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/bootstrap.h"

namespace memrpc {

struct SaBootstrapConfig {
  std::string service_name;
  std::string instance_name;
  bool lazy_connect = false;
};

class SaBootstrapChannel : public IBootstrapChannel {
 public:
  explicit SaBootstrapChannel(SaBootstrapConfig config = {});
  ~SaBootstrapChannel() override;

  SaBootstrapChannel(const SaBootstrapChannel&) = delete;
  SaBootstrapChannel& operator=(const SaBootstrapChannel&) = delete;
  SaBootstrapChannel(SaBootstrapChannel&&) noexcept = default;
  SaBootstrapChannel& operator=(SaBootstrapChannel&&) noexcept = default;

  const SaBootstrapConfig& config() const;
  const std::string& last_error() const;
  BootstrapHandles server_handles() const;

  StatusCode StartEngine() override;
  StatusCode Connect(BootstrapHandles* handles) override;
  StatusCode NotifyPeerRestarted() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memrpc

#endif  // MEMRPC_SA_BOOTSTRAP_H_
