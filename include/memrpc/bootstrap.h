#ifndef MEMRPC_BOOTSTRAP_H_
#define MEMRPC_BOOTSTRAP_H_

#include <cstdint>

#include "memrpc/types.h"

namespace memrpc {

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
};

}  // namespace memrpc

#endif  // MEMRPC_BOOTSTRAP_H_
