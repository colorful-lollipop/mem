#include "apps/minirpc/child/minirpc_service.h"

#include <chrono>
#include <thread>
#include <unistd.h>

namespace OHOS::Security::VirusProtectionService::MiniRpc {

EchoReply MiniRpcService::Echo(const EchoRequest& request) const {
  EchoReply reply;
  reply.text = request.text;
  return reply;
}

AddReply MiniRpcService::Add(const AddRequest& request) const {
  AddReply reply;
  reply.sum = request.lhs + request.rhs;
  return reply;
}

SleepReply MiniRpcService::Sleep(const SleepRequest& request) const {
  std::this_thread::sleep_for(std::chrono::milliseconds(request.delay_ms));
  SleepReply reply;
  reply.status = 0;
  return reply;
}

void MiniRpcService::RegisterHandlers(MemRpc::RpcServer* server) {
  if (server == nullptr) {
    return;
  }

  server->RegisterHandler(MemRpc::Opcode::MiniEcho,
                          [this](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            EchoRequest request;
                            if (!DecodeEchoRequest(call.payload, &request)) {
                              reply->status = MemRpc::StatusCode::ProtocolMismatch;
                              return;
                            }
                            EncodeEchoReply(Echo(request), &reply->payload);
                          });

  server->RegisterHandler(MemRpc::Opcode::MiniAdd,
                          [this](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            AddRequest request;
                            if (!DecodeAddRequest(call.payload, &request)) {
                              reply->status = MemRpc::StatusCode::ProtocolMismatch;
                              return;
                            }
                            EncodeAddReply(Add(request), &reply->payload);
                          });

  server->RegisterHandler(
      MemRpc::Opcode::MiniSleep,
      [this](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
        if (reply == nullptr) {
          return;
        }
        SleepRequest request;
        if (!DecodeSleepRequest(call.payload, &request)) {
          reply->status = MemRpc::StatusCode::ProtocolMismatch;
          return;
        }
        EncodeSleepReply(Sleep(request), &reply->payload);
      });

  // Test-only fault injection path for verifying client recovery.
  server->RegisterHandler(
      MemRpc::Opcode::MiniCrashForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
