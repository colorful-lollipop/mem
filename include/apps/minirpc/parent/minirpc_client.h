#ifndef APPS_MINIRPC_PARENT_MINIRPC_CLIENT_H_
#define APPS_MINIRPC_PARENT_MINIRPC_CLIENT_H_

#include <memory>

#include "apps/minirpc/parent/minirpc_async_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

class MiniRpcClient {
 public:
  explicit MiniRpcClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap = nullptr);
  ~MiniRpcClient();

  MiniRpcClient(const MiniRpcClient&) = delete;
  MiniRpcClient& operator=(const MiniRpcClient&) = delete;
  MiniRpcClient(MiniRpcClient&&) noexcept = default;
  MiniRpcClient& operator=(MiniRpcClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap);
  MemRpc::StatusCode Init();
  MemRpc::StatusCode Echo(const std::string& text, EchoReply* reply);
  MemRpc::StatusCode Add(int32_t lhs, int32_t rhs, AddReply* reply);
  MemRpc::StatusCode Sleep(uint32_t delay_ms,
                           SleepReply* reply,
                           MemRpc::Priority priority = MemRpc::Priority::Normal,
                           uint32_t exec_timeout_ms = 30000);
  void Shutdown();

 private:
  MiniRpcAsyncClient async_client_;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PARENT_MINIRPC_CLIENT_H_
