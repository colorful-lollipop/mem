#ifndef MEMRPC_SERVER_RPC_SERVER_H_
#define MEMRPC_SERVER_RPC_SERVER_H_

#include <memory>

#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/handler.h"

namespace memrpc {

struct ServerOptions {
  uint32_t high_worker_threads = 1;
  uint32_t normal_worker_threads = 1;
};

class RpcServer {
 public:
  RpcServer();
  explicit RpcServer(BootstrapHandles handles, ServerOptions options = {});
  ~RpcServer();

  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;
  RpcServer(RpcServer&&) noexcept = default;
  RpcServer& operator=(RpcServer&&) noexcept = default;

  void SetBootstrapHandles(BootstrapHandles handles);
  void RegisterHandler(Opcode opcode, RpcHandler handler);
  void SetOptions(ServerOptions options);
  StatusCode PublishEvent(const RpcEvent& event);
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

#endif  // MEMRPC_SERVER_RPC_SERVER_H_
