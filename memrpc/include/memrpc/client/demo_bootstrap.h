#ifndef MEMRPC_CLIENT_DEMO_BOOTSTRAP_H_
#define MEMRPC_CLIENT_DEMO_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/core/protocol.h"
#include "memrpc/core/bootstrap.h"

namespace memrpc {

struct DemoBootstrapConfig {
  uint32_t highRingSize = 32;
  uint32_t normalRingSize = 32;
  uint32_t responseRingSize = 64;
  uint32_t slotCount = 64;
  uint32_t maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  uint32_t maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES;
  std::string shmName;
};

class PosixDemoBootstrapChannel : public IBootstrapChannel {
 public:
  explicit PosixDemoBootstrapChannel(DemoBootstrapConfig config = {});
  ~PosixDemoBootstrapChannel() override;

  StatusCode OpenSession(BootstrapHandles& handles) override;
  StatusCode CloseSession() override;
  void SetEngineDeathCallback(EngineDeathCallback callback) override;

  BootstrapHandles serverHandles() const;
  void SimulateEngineDeathForTest(uint64_t session_id = 0);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CLIENT_DEMO_BOOTSTRAP_H_
