#include "apps/minirpc/parent/minirpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

MemRpc::StatusCode DecodeReplyStatus(const MemRpc::RpcReply& reply) {
  return reply.status;
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
  if (reply == nullptr) {
    return MemRpc::StatusCode::InvalidArgument;
  }
  EchoRequest request{text};
  MemRpc::RpcReply rpc_reply;
  const MemRpc::StatusCode status = async_client_.EchoAsync(request).Wait(&rpc_reply);
  if (status != MemRpc::StatusCode::Ok) {
    return status;
  }
  return DecodeEchoReply(rpc_reply.payload, reply) ? DecodeReplyStatus(rpc_reply)
                                                   : MemRpc::StatusCode::ProtocolMismatch;
}

MemRpc::StatusCode MiniRpcClient::Add(int32_t lhs, int32_t rhs, AddReply* reply) {
  if (reply == nullptr) {
    return MemRpc::StatusCode::InvalidArgument;
  }
  AddRequest request{lhs, rhs};
  MemRpc::RpcReply rpc_reply;
  const MemRpc::StatusCode status = async_client_.AddAsync(request).Wait(&rpc_reply);
  if (status != MemRpc::StatusCode::Ok) {
    return status;
  }
  return DecodeAddReply(rpc_reply.payload, reply) ? DecodeReplyStatus(rpc_reply)
                                                  : MemRpc::StatusCode::ProtocolMismatch;
}

MemRpc::StatusCode MiniRpcClient::Sleep(uint32_t delay_ms,
                                        SleepReply* reply,
                                        MemRpc::Priority priority,
                                        uint32_t exec_timeout_ms) {
  if (reply == nullptr) {
    return MemRpc::StatusCode::InvalidArgument;
  }
  SleepRequest request{delay_ms};
  MemRpc::RpcReply rpc_reply;
  const MemRpc::StatusCode status =
      async_client_.SleepAsync(request, priority, exec_timeout_ms).Wait(&rpc_reply);
  if (status != MemRpc::StatusCode::Ok) {
    return status;
  }
  return DecodeSleepReply(rpc_reply.payload, reply) ? DecodeReplyStatus(rpc_reply)
                                                    : MemRpc::StatusCode::ProtocolMismatch;
}

void MiniRpcClient::Shutdown() {
  async_client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
