#ifndef MEMRPC_COMPAT_ENGINE_SERVER_H_
#define MEMRPC_COMPAT_ENGINE_SERVER_H_

#include <memory>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/types.h"
#include "memrpc/server/handler.h"
#include "memrpc/server/rpc_server.h"

namespace memrpc {

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
  void RegisterHandler(Opcode opcode, RpcHandler handler);
  void SetOptions(ServerOptions options);
  StatusCode Start();
  void Run();
  void Stop();

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

#endif  // MEMRPC_COMPAT_ENGINE_SERVER_H_
