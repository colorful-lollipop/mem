#include "apps/minirpc/parent/minirpc_async_client.h"

#include <utility>

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

MemRpc::RpcCall MiniRpcAsyncClient::MakeCall(MemRpc::Opcode opcode,
                                             const std::vector<uint8_t>& payload,
                                             MemRpc::Priority priority,
                                             uint32_t exec_timeout_ms) const {
  MemRpc::RpcCall call;
  call.opcode = opcode;
  call.priority = priority;
  call.exec_timeout_ms = exec_timeout_ms;
  call.payload = payload;
  return call;
}

MemRpc::RpcFuture MiniRpcAsyncClient::EchoAsync(const EchoRequest& request) {
  std::vector<uint8_t> payload;
  if (!EncodeEchoRequest(request, &payload)) {
    return {};
  }
  return client_.InvokeAsync(MakeCall(MemRpc::Opcode::MiniEcho, payload));
}

MemRpc::RpcFuture MiniRpcAsyncClient::AddAsync(const AddRequest& request) {
  std::vector<uint8_t> payload;
  if (!EncodeAddRequest(request, &payload)) {
    return {};
  }
  return client_.InvokeAsync(MakeCall(MemRpc::Opcode::MiniAdd, payload));
}

MemRpc::RpcFuture MiniRpcAsyncClient::SleepAsync(const SleepRequest& request,
                                                 MemRpc::Priority priority,
                                                 uint32_t exec_timeout_ms) {
  std::vector<uint8_t> payload;
  if (!EncodeSleepRequest(request, &payload)) {
    return {};
  }
  return client_.InvokeAsync(MakeCall(MemRpc::Opcode::MiniSleep, payload, priority, exec_timeout_ms));
}

void MiniRpcAsyncClient::Shutdown() {
  client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
