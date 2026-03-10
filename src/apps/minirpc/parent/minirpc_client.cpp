#include "apps/minirpc/parent/minirpc_client.h"

#include "memrpc/client/typed_invoker.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

MiniRpcClient::MiniRpcClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
    : async_client_(std::move(bootstrap)) {}

MiniRpcClient::~MiniRpcClient() = default;

void MiniRpcClient::SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap) {
  async_client_.SetBootstrapChannel(std::move(bootstrap));
}

MemRpc::StatusCode MiniRpcClient::Init() {
  return async_client_.Init();
}

MemRpc::StatusCode MiniRpcClient::Echo(const std::string& text, EchoReply* reply) {
  EchoRequest request{text};
  return MemRpc::WaitAndDecode(async_client_.EchoAsync(request), reply);
}

MemRpc::StatusCode MiniRpcClient::Add(int32_t lhs, int32_t rhs, AddReply* reply) {
  AddRequest request{lhs, rhs};
  return MemRpc::WaitAndDecode(async_client_.AddAsync(request), reply);
}

MemRpc::StatusCode MiniRpcClient::Sleep(uint32_t delay_ms,
                                        SleepReply* reply,
                                        MemRpc::Priority priority,
                                        uint32_t exec_timeout_ms) {
  SleepRequest request{delay_ms};
  return MemRpc::WaitAndDecode(async_client_.SleepAsync(request, priority, exec_timeout_ms), reply);
}

void MiniRpcClient::Shutdown() {
  async_client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
