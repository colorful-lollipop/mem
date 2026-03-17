#ifndef MEMRPC_CLIENT_DEV_BOOTSTRAP_H_
#define MEMRPC_CLIENT_DEV_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/core/protocol.h"
#include "memrpc/core/bootstrap.h"

namespace MemRpc {

struct DevBootstrapConfig {
  uint32_t highRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.highRingSize;
  uint32_t normalRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.normalRingSize;
  uint32_t responseRingSize = DEFAULT_SHARED_MEMORY_LAYOUT.responseRingSize;
  uint32_t maxRequestBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxRequestBytes;
  uint32_t maxResponseBytes = DEFAULT_SHARED_MEMORY_LAYOUT.maxResponseBytes;
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
