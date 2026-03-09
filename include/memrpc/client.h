#ifndef MEMRPC_CLIENT_H_
#define MEMRPC_CLIENT_H_

#include <memory>

#include "memrpc/bootstrap.h"
#include "memrpc/types.h"

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
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memrpc

#endif  // MEMRPC_CLIENT_H_
