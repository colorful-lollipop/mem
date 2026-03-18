#include "testkit/testkit_service.h"

#include <chrono>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "testkit/testkit_protocol.h"

namespace VirusExecutorService::testkit {

namespace {
template <typename Registrar, typename Req, typename Rep, typename Handler>
void RegisterTypedServiceHandler(Registrar* registrar, MemRpc::Opcode opcode, Handler handler)
{
    if (registrar == nullptr) {
        return;
    }
    registrar->RegisterHandler(
        opcode,
        [h = std::move(handler)](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
            if (reply == nullptr) {
                return;
            }
            Req request;
            if (!MemRpc::DecodeMessage<Req>(call.payload, &request)) {
                reply->status = MemRpc::StatusCode::ProtocolMismatch;
                return;
            }
            if (!MemRpc::EncodeMessage<Rep>(h(request), &reply->payload)) {
                reply->status = MemRpc::StatusCode::EngineInternalError;
                reply->payload.clear();
            }
        });
}

template <typename Registrar>
void RegisterCoreHandlers(Registrar* registrar, TestkitService* service)
{
    RegisterTypedServiceHandler<Registrar, EchoRequest, EchoReply>(
        registrar,
        static_cast<MemRpc::Opcode>(TestkitOpcode::Echo),
        [service](const EchoRequest& request) { return service->Echo(request); });
    RegisterTypedServiceHandler<Registrar, AddRequest, AddReply>(
        registrar,
        static_cast<MemRpc::Opcode>(TestkitOpcode::Add),
        [service](const AddRequest& request) { return service->Add(request); });
    RegisterTypedServiceHandler<Registrar, SleepRequest, SleepReply>(
        registrar,
        static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep),
        [service](const SleepRequest& request) { return service->Sleep(request); });
}

template <typename Registrar>
void RegisterFaultInjectionHandlers(Registrar* registrar)
{
    if (registrar == nullptr) {
        return;
    }

    registrar->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) { _exit(99); });

    registrar->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::HangForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
            while (true) {
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        });

    registrar->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::OomForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
            std::vector<std::vector<char>> leaks;
            while (true) {
                leaks.emplace_back(64 * 1024 * 1024, 'X');
            }
        });

    registrar->RegisterHandler(
        static_cast<MemRpc::Opcode>(TestkitOpcode::StackOverflowForTest),
        [](const MemRpc::RpcServerCall&, MemRpc::RpcServerReply*) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
            struct Recurse {
                static void Go(volatile int depth) { Go(depth + 1); }
            };
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            Recurse::Go(0);
        });
}

template <typename Registrar>
void RegisterAllHandlers(Registrar* registrar, TestkitService* service, bool enableFaultInjection)
{
    RegisterCoreHandlers(registrar, service);
    if (enableFaultInjection) {
        RegisterFaultInjectionHandlers(registrar);
    }
}

}  // namespace

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

void TestkitService::RegisterHandlers(RpcHandlerSink* sink) {
    RegisterAllHandlers(sink, this, options_.enableFaultInjection);
}

}  // namespace VirusExecutorService::testkit
