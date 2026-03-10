#ifndef MEMRPC_CLIENT_DEMO_BOOTSTRAP_H_
#define MEMRPC_CLIENT_DEMO_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "core/protocol.h"
#include "memrpc/core/bootstrap.h"

namespace memrpc {

struct DemoBootstrapConfig {
  uint32_t high_ring_size = 32;
  uint32_t normal_ring_size = 32;
  uint32_t response_ring_size = 64;
  uint32_t slot_count = 64;
  uint32_t max_request_bytes = kDefaultMaxRequestBytes;
  uint32_t max_response_bytes = kDefaultMaxResponseBytes;
  std::string shm_name;
};

class PosixDemoBootstrapChannel : public IBootstrapChannel {
 public:
  explicit PosixDemoBootstrapChannel(DemoBootstrapConfig config = {});
  ~PosixDemoBootstrapChannel() override;

  StatusCode StartEngine() override;
  StatusCode Connect(BootstrapHandles* handles) override;
  StatusCode NotifyPeerRestarted() override;
  void SetEngineDeathCallback(EngineDeathCallback callback) override;

  BootstrapHandles server_handles() const;
  void SimulateEngineDeathForTest(uint64_t session_id = 0);
  void SetDupFailureAfterCountForTest(int count);

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
