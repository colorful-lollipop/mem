#ifndef MEMRPC_DEMO_BOOTSTRAP_H_
#define MEMRPC_DEMO_BOOTSTRAP_H_

#include <memory>
#include <string>

#include "memrpc/bootstrap.h"

namespace memrpc {

struct DemoBootstrapConfig {
  uint32_t high_ring_size = 64;
  uint32_t normal_ring_size = 256;
  uint32_t response_ring_size = 256;
  uint32_t slot_count = 128;
  std::string shm_name;
};

class PosixDemoBootstrapChannel : public IBootstrapChannel {
 public:
  explicit PosixDemoBootstrapChannel(DemoBootstrapConfig config = {});
  ~PosixDemoBootstrapChannel() override;

  StatusCode StartEngine() override;
  StatusCode Connect(BootstrapHandles* handles) override;
  StatusCode NotifyPeerRestarted() override;

  BootstrapHandles server_handles() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace memrpc

#endif  // MEMRPC_DEMO_BOOTSTRAP_H_
