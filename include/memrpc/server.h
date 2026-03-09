#ifndef MEMRPC_SERVER_H_
#define MEMRPC_SERVER_H_

#include <memory>

#include "memrpc/bootstrap.h"
#include "memrpc/handler.h"
#include "memrpc/types.h"

namespace memrpc {

struct ServerOptions {
  uint32_t high_worker_threads = 1;
  uint32_t normal_worker_threads = 1;
};

class EngineServer {
 public:
  EngineServer();
  EngineServer(BootstrapHandles handles,
               std::shared_ptr<IScanHandler> handler,
               ServerOptions options = {});
  ~EngineServer();

  EngineServer(const EngineServer&) = delete;
  EngineServer& operator=(const EngineServer&) = delete;
  EngineServer(EngineServer&&) noexcept = default;
  EngineServer& operator=(EngineServer&&) noexcept = default;

  void SetBootstrapHandles(BootstrapHandles handles);
  void SetScanHandler(std::shared_ptr<IScanHandler> handler);
  void SetOptions(ServerOptions options);
  StatusCode Start();
  void Run();
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memrpc

#endif  // MEMRPC_SERVER_H_
