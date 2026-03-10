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

void MiniRpcService::RegisterHandlers(MemRpc::RpcServer* server, DecodeMode mode) {
  if (server == nullptr) {
    return;
  }

  const bool use_view = (mode == DecodeMode::View);

  server->RegisterHandler(MemRpc::Opcode::MiniEcho,
                          [this, use_view](const MemRpc::RpcServerCall& call,
                                           MemRpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            EchoReply echo_reply;
                            if (use_view) {
                              EchoRequestView request;
                              if (!DecodeMessageView<EchoRequestView>(call.payload, &request)) {
                                reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                return;
                              }
                              echo_reply.text.assign(request.text);
                            } else {
                              EchoRequest request;
                              if (!DecodeMessage<EchoRequest>(call.payload, &request)) {
                                reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                return;
                              }
                              echo_reply = Echo(request);
                            }
                            EncodeMessage<EchoReply>(echo_reply, &reply->payload);
                          });

  server->RegisterHandler(MemRpc::Opcode::MiniAdd,
                          [this, use_view](const MemRpc::RpcServerCall& call,
                                           MemRpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            AddRequest request;
                            if (use_view) {
                              AddRequestView view_request;
                              if (!DecodeMessageView<AddRequestView>(call.payload, &view_request)) {
                                reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                return;
                              }
                              request.lhs = view_request.lhs;
                              request.rhs = view_request.rhs;
                            } else {
                              if (!DecodeMessage<AddRequest>(call.payload, &request)) {
                                reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                return;
                              }
                            }
                            EncodeMessage<AddReply>(Add(request), &reply->payload);
                          });

  server->RegisterHandler(
      MemRpc::Opcode::MiniSleep,
      [this, use_view](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
        if (reply == nullptr) {
          return;
        }
        SleepRequest request;
        if (use_view) {
          SleepRequestView view_request;
          if (!DecodeMessageView<SleepRequestView>(call.payload, &view_request)) {
            reply->status = MemRpc::StatusCode::ProtocolMismatch;
            return;
          }
          request.delay_ms = view_request.delay_ms;
        } else {
          if (!DecodeMessage<SleepRequest>(call.payload, &request)) {
            reply->status = MemRpc::StatusCode::ProtocolMismatch;
            return;
          }
        }
        EncodeMessage<SleepReply>(Sleep(request), &reply->payload);
      });

  // Test-only fault injection path for verifying client recovery.
  server->RegisterHandler(
      MemRpc::Opcode::MiniCrashForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
