#ifndef MEMRPC_CORE_BOOTSTRAP_H_
#define MEMRPC_CORE_BOOTSTRAP_H_

#include <cstdint>
#include <functional>

#include "memrpc/core/types.h"

namespace memrpc {

using EngineDeathCallback = std::function<void(uint64_t)>;

struct BootstrapHandles {
  int shm_fd = -1;
  int high_req_event_fd = -1;
  int normal_req_event_fd = -1;
  int resp_event_fd = -1;
  uint32_t protocol_version = 0;
  uint64_t session_id = 0;
};

class IBootstrapChannel {
 public:
  virtual ~IBootstrapChannel() = default;

  virtual StatusCode StartEngine() = 0;
  virtual StatusCode Connect(BootstrapHandles* handles) = 0;
  virtual StatusCode NotifyPeerRestarted() = 0;
  virtual void SetEngineDeathCallback(EngineDeathCallback callback) = 0;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CORE_BOOTSTRAP_H_
