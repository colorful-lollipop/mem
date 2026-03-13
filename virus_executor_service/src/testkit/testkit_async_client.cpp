#include "virus_executor_service/testkit/testkit_async_client.h"

#include <utility>

#include "memrpc/client/typed_invoker.h"
#include "virus_executor_service/testkit/testkit_protocol.h"

namespace virus_executor_service::testkit {

TestkitAsyncClient::TestkitAsyncClient(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap)) {}

TestkitAsyncClient::~TestkitAsyncClient() = default;

void TestkitAsyncClient::SetBootstrapChannel(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap) {
    client_.SetBootstrapChannel(std::move(bootstrap));
}

memrpc::StatusCode TestkitAsyncClient::Init() {
    return client_.Init();
}

memrpc::TypedFuture<EchoReply> TestkitAsyncClient::EchoAsync(const EchoRequest& request) {
    return memrpc::InvokeTypedAsync<EchoRequest, EchoReply>(
        &client_, static_cast<memrpc::Opcode>(TestkitOpcode::Echo), request);
}

memrpc::TypedFuture<AddReply> TestkitAsyncClient::AddAsync(const AddRequest& request) {
    return memrpc::InvokeTypedAsync<AddRequest, AddReply>(
        &client_, static_cast<memrpc::Opcode>(TestkitOpcode::Add), request);
}

memrpc::TypedFuture<SleepReply> TestkitAsyncClient::SleepAsync(
    const SleepRequest& request,
    memrpc::Priority priority,
    uint32_t execTimeoutMs) {
    return memrpc::InvokeTypedAsync<SleepRequest, SleepReply>(
        &client_, static_cast<memrpc::Opcode>(TestkitOpcode::Sleep), request, priority,
        execTimeoutMs);
}

void TestkitAsyncClient::Shutdown() {
    client_.Shutdown();
}

}  // namespace virus_executor_service::testkit
