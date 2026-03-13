#include "testkit/testkit_service.h"

#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/server/typed_handler.h"
#include "testkit/testkit_protocol.h"

namespace virus_executor_service::testkit {

TestkitService::TestkitService(TestkitServiceOptions options)
    : options_(options) {}

void TestkitService::SetFaultInjectionEnabled(bool enabled) {
    options_.enableFaultInjection = enabled;
}

EchoReply TestkitService::Echo(const EchoRequest& request) const {
    EchoReply reply;
    reply.text = request.text;
    return reply;
}

AddReply TestkitService::Add(const AddRequest& request) const {
    AddReply reply;
    reply.sum = static_cast<int32_t>(
        static_cast<uint32_t>(request.lhs) + static_cast<uint32_t>(request.rhs));
    return reply;
}

SleepReply TestkitService::Sleep(const SleepRequest& request) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(request.delayMs));
    SleepReply reply;
    reply.status = 0;
    return reply;
}

void TestkitService::RegisterHandlers(MemRpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    MemRpc::RegisterTypedHandler<EchoRequest, EchoReply>(
        server, static_cast<MemRpc::Opcode>(TestkitOpcode::Echo),
        [this](const EchoRequest& request) { return Echo(request); });
    MemRpc::RegisterTypedHandler<AddRequest, AddReply>(
        server, static_cast<MemRpc::Opcode>(TestkitOpcode::Add),
        [this](const AddRequest& request) { return Add(request); });
    MemRpc::RegisterTypedHandler<SleepRequest, SleepReply>(
        server, static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep),
        [this](const SleepRequest& request) { return Sleep(request); });

    if (!options_.enableFaultInjection) {
        return;
    }

    // Fault injection stays opt-in so the default demo engine does not expose
    // crash endpoints unless tests explicitly request them.
    server->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });

    server->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::HangForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
            while (true) {
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        });

    server->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::OomForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
            std::vector<std::vector<char>> leaks;
            while (true) {
                leaks.emplace_back(64 * 1024 * 1024, 'X');
            }
        });

    server->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::StackOverflowForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
            struct Recurse {
                static void Go(volatile int depth) { Go(depth + 1); }
            };
            Recurse::Go(0);
        });
}

}  // namespace virus_executor_service::testkit
