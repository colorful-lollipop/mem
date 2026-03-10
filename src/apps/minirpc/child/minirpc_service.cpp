#include "apps/minirpc/child/minirpc_service.h"

#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/server/typed_handler.h"

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

  MemRpc::RegisterTypedHandler<EchoRequest, EchoReply>(
      server, MemRpc::Opcode::MiniEcho,
      [this](const EchoRequest& r) { return Echo(r); });
  MemRpc::RegisterTypedHandler<AddRequest, AddReply>(
      server, MemRpc::Opcode::MiniAdd,
      [this](const AddRequest& r) { return Add(r); });
  MemRpc::RegisterTypedHandler<SleepRequest, SleepReply>(
      server, MemRpc::Opcode::MiniSleep,
      [this](const SleepRequest& r) { return Sleep(r); });

  // Test-only fault injection paths for verifying client recovery.
  server->RegisterHandler(
      MemRpc::Opcode::MiniCrashForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });

  server->RegisterHandler(
      MemRpc::Opcode::MiniHangForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        while (true) {
          std::this_thread::sleep_for(std::chrono::hours(1));
        }
      });

  server->RegisterHandler(
      MemRpc::Opcode::MiniOomForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        std::vector<std::vector<char>> leaks;
        while (true) {
          leaks.emplace_back(64 * 1024 * 1024, 'X');
        }
      });

  server->RegisterHandler(
      MemRpc::Opcode::MiniStackOverflowForTest,
      [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
        struct Recurse {
          static void Go(volatile int depth) { Go(depth + 1); }
        };
        Recurse::Go(0);
      });
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
