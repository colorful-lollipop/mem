#ifndef MEMRPC_CLIENT_DEV_BOOTSTRAP_H_
#define MEMRPC_CLIENT_DEV_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/core/protocol.h"
#include "memrpc/core/bootstrap.h"

namespace MemRpc {

struct DevBootstrapConfig {
  uint32_t highRingSize = 32;
  uint32_t normalRingSize = 32;
  uint32_t responseRingSize = 64;
  uint32_t slotCount = 64;
  uint32_t maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  uint32_t maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;
  std::string shmName;
};

class DevBootstrapChannel : public IBootstrapChannel {
 public:
  explicit DevBootstrapChannel(DevBootstrapConfig config = {});
  ~DevBootstrapChannel() override;

  StatusCode OpenSession(BootstrapHandles& handles) override;
  StatusCode CloseSession() override;
  ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
  void SetEngineDeathCallback(EngineDeathCallback callback) override;

  [[nodiscard]] BootstrapHandles serverHandles() const;
  void SimulateEngineDeathForTest(uint64_t session_id = 0);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CLIENT_DEV_BOOTSTRAP_H_
