#ifndef MEMRPC_CLIENT_SA_BOOTSTRAP_H_
#define MEMRPC_CLIENT_SA_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/core/bootstrap.h"

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
  void SimulateEngineDeathForTest(uint64_t session_id = 0);

  StatusCode OpenSession(BootstrapHandles& handles) override;
  StatusCode CloseSession() override;
  void SetEngineDeathCallback(EngineDeathCallback callback) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CLIENT_SA_BOOTSTRAP_H_
