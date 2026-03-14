#ifndef MEMRPC_CLIENT_SA_BOOTSTRAP_H_
#define MEMRPC_CLIENT_SA_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/core/bootstrap.h"

namespace MemRpc {

struct SaBootstrapConfig {
  std::string serviceName;
  std::string instanceName;
  bool lazyConnect = false;
};

class SaBootstrapChannel : public IBootstrapChannel {
 public:
  explicit SaBootstrapChannel(SaBootstrapConfig config = {});
  ~SaBootstrapChannel() override;

  SaBootstrapChannel(const SaBootstrapChannel&) = delete;
  SaBootstrapChannel& operator=(const SaBootstrapChannel&) = delete;
  SaBootstrapChannel(SaBootstrapChannel&&) noexcept = default;
  SaBootstrapChannel& operator=(SaBootstrapChannel&&) noexcept = default;

  [[nodiscard]] const SaBootstrapConfig& Config() const;
  [[nodiscard]] const std::string& LastError() const;
  [[nodiscard]] BootstrapHandles ServerHandles() const;
  void SimulateEngineDeathForTest(uint64_t session_id = 0);

  StatusCode OpenSession(BootstrapHandles& handles) override;
  StatusCode CloseSession() override;
  void SetEngineDeathCallback(EngineDeathCallback callback) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CLIENT_SA_BOOTSTRAP_H_
