#ifndef MEMRPC_SERVER_RPC_SERVER_H_
#define MEMRPC_SERVER_RPC_SERVER_H_

#include <memory>

#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/handler.h"

namespace memrpc {

class TaskExecutor;

struct ServerOptions {
  // 高优与普通请求各自拥有独立 worker 池，避免普通队列拖慢高优请求。
  uint32_t highWorkerThreads = 1;
  uint32_t normalWorkerThreads = 1;
  // response writer 前允许同时挂起的 completion 数量；0 表示按 response ring 容量取默认值。
  uint32_t completionQueueCapacity = 0;
  // 自定义 executor；若为空则使用内置 ThreadPoolExecutor。
  std::shared_ptr<TaskExecutor> highExecutor;
  std::shared_ptr<TaskExecutor> normalExecutor;
};

struct RpcServerRuntimeStats {
  uint32_t completionBacklog = 0;
  uint32_t completionBacklogCapacity = 0;
  uint32_t highRequestRingPending = 0;
  uint32_t normalRequestRingPending = 0;
  uint32_t responseRingPending = 0;
  bool waitingForResponseCredit = false;
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
  // 重复注册同一 opcode 会覆盖旧 handler；传入空 handler 等价于注销。
  void RegisterHandler(Opcode opcode, RpcHandler handler);
  void SetOptions(ServerOptions options);
  // PublishEvent 复用 response ring + response slot，把异步事件发给客户端。
  StatusCode PublishEvent(const RpcEvent& event);
  // Start 负责 attach session、拉起 worker 池和请求分发线程。
  StatusCode Start();
  // Run 用于 demo/守护式场景，内部会保持 server 存活直到 Stop。
  void Run();
  RpcServerRuntimeStats GetRuntimeStats() const;
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
