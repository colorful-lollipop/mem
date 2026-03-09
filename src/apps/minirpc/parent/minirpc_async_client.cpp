#include "apps/minirpc/parent/minirpc_async_client.h"

#include <utility>

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

MemRpc::RpcCall MakeCall(MemRpc::Opcode opcode,
                         std::vector<uint8_t> payload,
                         MemRpc::Priority priority = MemRpc::Priority::Normal,
                         uint32_t exec_timeout_ms = 30000) {
  MemRpc::RpcCall call;
  call.opcode = opcode;
  call.priority = priority;
  call.exec_timeout_ms = exec_timeout_ms;
  call.payload = std::move(payload);
  return call;
}

template <typename Request>
MemRpc::RpcFuture InvokeEncoded(MemRpc::RpcClient* client,
                                const Request& request,
                                MemRpc::Opcode opcode,
                                MemRpc::Priority priority = MemRpc::Priority::Normal,
                                uint32_t exec_timeout_ms = 30000) {
  std::vector<uint8_t> payload;
  if (!EncodeMessage<Request>(request, &payload)) {
    return {};
  }
  return client->InvokeAsync(MakeCall(opcode, std::move(payload), priority, exec_timeout_ms));
}

}  // namespace

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
  return InvokeEncoded(&client_, request, MemRpc::Opcode::MiniEcho);
}

MemRpc::RpcFuture MiniRpcAsyncClient::AddAsync(const AddRequest& request) {
  return InvokeEncoded(&client_, request, MemRpc::Opcode::MiniAdd);
}

MemRpc::RpcFuture MiniRpcAsyncClient::SleepAsync(const SleepRequest& request,
                                                 MemRpc::Priority priority,
                                                 uint32_t exec_timeout_ms) {
  return InvokeEncoded(&client_, request, MemRpc::Opcode::MiniSleep, priority, exec_timeout_ms);
}

void MiniRpcAsyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
