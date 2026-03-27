#include "testkit/testkit_service.h"

#include <unistd.h>
#include <chrono>
#include <thread>
#include <vector>

#include "testkit/testkit_protocol.h"
#include "virus_protection_executor_log.h"

namespace VirusExecutorService::testkit {

namespace {
void RegisterCoreHandlers(RpcHandlerSink* sink, TestkitService* service)
{
    RegisterTypedHandler<EchoRequest, EchoReply>(
        sink,
        static_cast<MemRpc::Opcode>(TestkitOpcode::Echo),
        [service](const EchoRequest& request) { return service->Echo(request); });
    RegisterTypedHandler<AddRequest, AddReply>(sink,
                                               static_cast<MemRpc::Opcode>(TestkitOpcode::Add),
                                               [service](const AddRequest& request) { return service->Add(request); });
    RegisterTypedHandler<SleepRequest, SleepReply>(
        sink,
        static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep),
        [service](const SleepRequest& request) { return service->Sleep(request); });
}

void RegisterFaultInjectionHandlers(RpcHandlerSink* sink)
{
    if (sink == nullptr) {
        HILOGE("RegisterFaultInjectionHandlers failed: sink is null");
        return;
    }

    sink->RegisterHandler(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest),
                          [](const MemRpc::RpcServerCall&, MemRpc::RpcReply*) { _exit(99); });

    sink->RegisterHandler(static_cast<MemRpc::Opcode>(TestkitOpcode::HangForTest),
                          [](const MemRpc::RpcServerCall&, MemRpc::RpcReply*) {
                              while (true) {
                                  std::this_thread::sleep_for(std::chrono::hours(1));
                              }
                          });

    sink->RegisterHandler(static_cast<MemRpc::Opcode>(TestkitOpcode::OomForTest),
                          [](const MemRpc::RpcServerCall&, MemRpc::RpcReply*) {
                              std::vector<std::vector<char>> leaks;
                              while (true) {
                                  leaks.emplace_back(64 * 1024 * 1024, 'X');
                              }
                          });

    sink->RegisterHandler(static_cast<MemRpc::Opcode>(TestkitOpcode::StackOverflowForTest),
                          [](const MemRpc::RpcServerCall&, MemRpc::RpcReply*) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
                              struct Recurse {
                                  static void Go(volatile int depth)
                                  {
                                      Go(depth + 1);
                                  }
                              };
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
                              Recurse::Go(0);
                          });
}

void RegisterAllHandlers(RpcHandlerSink* sink, TestkitService* service, bool enableFaultInjection)
{
    RegisterCoreHandlers(sink, service);
    if (enableFaultInjection) {
        RegisterFaultInjectionHandlers(sink);
    }
}

}  // namespace

TestkitService::TestkitService(TestkitServiceOptions options)
    : options_(options)
{
}

void TestkitService::SetFaultInjectionEnabled(bool enabled)
{
    options_.enableFaultInjection = enabled;
}

EchoReply TestkitService::Echo(const EchoRequest& request) const
{
    EchoReply reply;
    reply.text = request.text;
    return reply;
}

AddReply TestkitService::Add(const AddRequest& request) const
{
    AddReply reply;
    reply.sum = static_cast<int32_t>(static_cast<uint32_t>(request.lhs) + static_cast<uint32_t>(request.rhs));
    return reply;
}

SleepReply TestkitService::Sleep(const SleepRequest& request) const
{
    std::this_thread::sleep_for(std::chrono::milliseconds(request.delayMs));
    SleepReply reply;
    reply.status = 0;
    return reply;
}

void TestkitService::RegisterHandlers(RpcHandlerSink* sink)
{
    if (sink == nullptr) {
        HILOGE("TestkitService::RegisterHandlers failed: sink is null");
        return;
    }
    RegisterAllHandlers(sink, this, options_.enableFaultInjection);
}

}  // namespace VirusExecutorService::testkit
