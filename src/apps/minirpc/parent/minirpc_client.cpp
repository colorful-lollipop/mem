#include "apps/minirpc/parent/minirpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

MemRpc::StatusCode DecodeReplyStatus(const MemRpc::RpcReply& reply) {
  return reply.status;
}

template <typename Reply>
MemRpc::StatusCode WaitAndDecode(MemRpc::RpcFuture future, Reply* reply) {
  if (reply == nullptr) {
    return MemRpc::StatusCode::InvalidArgument;
  }

  MemRpc::RpcReply rpcReply;
  const MemRpc::StatusCode status = future.Wait(&rpcReply);
  if (status != MemRpc::StatusCode::Ok) {
    return status;
  }

  return DecodeMessage<Reply>(rpcReply.payload, reply) ? DecodeReplyStatus(rpcReply)
                                                       : MemRpc::StatusCode::ProtocolMismatch;
}

}  // namespace

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
  return WaitAndDecode(async_client_.EchoAsync(request), reply);
}

MemRpc::StatusCode MiniRpcClient::Add(int32_t lhs, int32_t rhs, AddReply* reply) {
  AddRequest request{lhs, rhs};
  return WaitAndDecode(async_client_.AddAsync(request), reply);
}

MemRpc::StatusCode MiniRpcClient::Sleep(uint32_t delay_ms,
                                        SleepReply* reply,
                                        MemRpc::Priority priority,
                                        uint32_t exec_timeout_ms) {
  SleepRequest request{delay_ms};
  return WaitAndDecode(async_client_.SleepAsync(request, priority, exec_timeout_ms), reply);
}

void MiniRpcClient::Shutdown() {
  async_client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
