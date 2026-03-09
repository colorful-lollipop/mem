#ifndef APPS_MINIRPC_PARENT_MINIRPC_ASYNC_CLIENT_H_
#define APPS_MINIRPC_PARENT_MINIRPC_ASYNC_CLIENT_H_

#include <functional>
#include <memory>

#include "apps/minirpc/common/minirpc_codec.h"
#include "memrpc/client/rpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

class MiniRpcAsyncClient {
 public:
  explicit MiniRpcAsyncClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap = nullptr);
  ~MiniRpcAsyncClient();

  MiniRpcAsyncClient(const MiniRpcAsyncClient&) = delete;
  MiniRpcAsyncClient& operator=(const MiniRpcAsyncClient&) = delete;
  MiniRpcAsyncClient(MiniRpcAsyncClient&&) noexcept = default;
  MiniRpcAsyncClient& operator=(MiniRpcAsyncClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap);
  MemRpc::StatusCode Init();
  MemRpc::RpcFuture EchoAsync(const EchoRequest& request);
  MemRpc::RpcFuture AddAsync(const AddRequest& request);
  MemRpc::RpcFuture SleepAsync(const SleepRequest& request,
                               MemRpc::Priority priority = MemRpc::Priority::Normal,
                               uint32_t exec_timeout_ms = 30000);
  void Shutdown();

 private:
  MemRpc::RpcCall MakeCall(MemRpc::Opcode opcode,
                           const std::vector<uint8_t>& payload,
                           MemRpc::Priority priority = MemRpc::Priority::Normal,
                           uint32_t exec_timeout_ms = 30000) const;

  MemRpc::RpcClient client_;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PARENT_MINIRPC_ASYNC_CLIENT_H_
