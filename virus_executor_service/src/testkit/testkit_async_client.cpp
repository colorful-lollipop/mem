#include "testkit/testkit_async_client.h"

#include <utility>

#include "memrpc/client/typed_invoker.h"
#include "testkit/testkit_protocol.h"

namespace VirusExecutorService::testkit {

TestkitAsyncClient::TestkitAsyncClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap)) {}

TestkitAsyncClient::~TestkitAsyncClient() = default;

void TestkitAsyncClient::SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap) {
    client_.SetBootstrapChannel(std::move(bootstrap));
}

MemRpc::StatusCode TestkitAsyncClient::Init() {
    return client_.Init();
}

MemRpc::TypedFuture<EchoReply> TestkitAsyncClient::EchoAsync(const EchoRequest& request) {
    return MemRpc::InvokeTypedAsync<EchoRequest, EchoReply>(
        &client_, static_cast<MemRpc::Opcode>(TestkitOpcode::Echo), request);
}

MemRpc::TypedFuture<AddReply> TestkitAsyncClient::AddAsync(const AddRequest& request) {
    return MemRpc::InvokeTypedAsync<AddRequest, AddReply>(
        &client_, static_cast<MemRpc::Opcode>(TestkitOpcode::Add), request);
}

MemRpc::TypedFuture<SleepReply> TestkitAsyncClient::SleepAsync(
    const SleepRequest& request,
    MemRpc::Priority priority,
    uint32_t execTimeoutMs) {
    return MemRpc::InvokeTypedAsync<SleepRequest, SleepReply>(
        &client_, static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep), request, priority,
        execTimeoutMs);
}

void TestkitAsyncClient::Shutdown() {
    client_.Shutdown();
}

}  // namespace VirusExecutorService::testkit
