#ifndef MEMRPC_COMPAT_ENGINE_CLIENT_H_
#define MEMRPC_COMPAT_ENGINE_CLIENT_H_

#include <memory>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"

namespace memrpc {

class EngineClient {
 public:
  explicit EngineClient(std::shared_ptr<IBootstrapChannel> bootstrap = nullptr);
  ~EngineClient();

  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;
  EngineClient(EngineClient&&) noexcept = default;
  EngineClient& operator=(EngineClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap);
  StatusCode Init();
  StatusCode Scan(const ScanRequest& request, ScanResult* result);
  StatusCode ScanBehavior(const ScanBehaviorRequest& request, ScanBehaviorResult* result);
  void Shutdown();

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

#endif  // MEMRPC_COMPAT_ENGINE_CLIENT_H_
