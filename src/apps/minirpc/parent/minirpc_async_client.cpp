#include "apps/minirpc/parent/minirpc_async_client.h"

#include <utility>

#include "memrpc/client/typed_invoker.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

MiniRpcAsyncClient::MiniRpcAsyncClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap)) {}

MiniRpcAsyncClient::~MiniRpcAsyncClient() = default;

void MiniRpcAsyncClient::SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap) {
  client_.SetBootstrapChannel(std::move(bootstrap));
}

MemRpc::StatusCode MiniRpcAsyncClient::Init() {
  return client_.Init();
}

MemRpc::RpcFuture MiniRpcAsyncClient::EchoAsync(const EchoRequest& request) {
  return MemRpc::InvokeTyped(&client_, MemRpc::Opcode::MiniEcho, request);
}

MemRpc::RpcFuture MiniRpcAsyncClient::AddAsync(const AddRequest& request) {
  return MemRpc::InvokeTyped(&client_, MemRpc::Opcode::MiniAdd, request);
}

MemRpc::RpcFuture MiniRpcAsyncClient::SleepAsync(const SleepRequest& request,
                                                 MemRpc::Priority priority,
                                                 uint32_t exec_timeout_ms) {
  return MemRpc::InvokeTyped(&client_, MemRpc::Opcode::MiniSleep, request, priority, exec_timeout_ms);
}

void MiniRpcAsyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
