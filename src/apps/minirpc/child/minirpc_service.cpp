#include "apps/minirpc/child/minirpc_service.h"

#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

#include "apps/minirpc/protocol.h"
#include "memrpc/server/typed_handler.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

EchoReply MiniRpcService::Echo(const EchoRequest& request) const {
  EchoReply reply;
  reply.text = request.text;
  return reply;
}

AddReply MiniRpcService::Add(const AddRequest& request) const {
  AddReply reply;
  // Use unsigned arithmetic to avoid signed overflow UB, then cast back.
  reply.sum = static_cast<int32_t>(
      static_cast<uint32_t>(request.lhs) + static_cast<uint32_t>(request.rhs));
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

  MemRpc::RegisterTypedHandler<EchoRequest, EchoReply>(
      server, static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniEcho),
      [this](const EchoRequest& r) { return Echo(r); });
  MemRpc::RegisterTypedHandler<AddRequest, AddReply>(
      server, static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniAdd),
      [this](const AddRequest& r) { return Add(r); });
  MemRpc::RegisterTypedHandler<SleepRequest, SleepReply>(
      server, static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniSleep),
      [this](const SleepRequest& r) { return Sleep(r); });

  // Test-only fault injection paths for verifying client recovery.
  server->RegisterHandler(
      static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest),
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });

  server->RegisterHandler(
      static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniHangForTest),
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        while (true) {
          std::this_thread::sleep_for(std::chrono::hours(1));
        }
      });

  server->RegisterHandler(
      static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniOomForTest),
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        std::vector<std::vector<char>> leaks;
        while (true) {
          leaks.emplace_back(64 * 1024 * 1024, 'X');
        }
      });

  server->RegisterHandler(
      static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniStackOverflowForTest),
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        struct Recurse {
          static void Go(volatile int depth) { Go(depth + 1); }
        };
        Recurse::Go(0);
      });
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
