#include "vpsdemo/testkit/testkit_client.h"

#include <utility>

namespace vpsdemo::testkit {

TestkitClient::TestkitClient(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap)
    : asyncClient_(std::move(bootstrap)) {}

TestkitClient::~TestkitClient() = default;

void TestkitClient::SetBootstrapChannel(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap) {
    asyncClient_.SetBootstrapChannel(std::move(bootstrap));
}

memrpc::StatusCode TestkitClient::Init() {
    return asyncClient_.Init();
}

memrpc::StatusCode TestkitClient::Echo(const std::string& text, EchoReply* reply) {
    EchoRequest request{text};
    return asyncClient_.EchoAsync(request).Wait(reply);
}

memrpc::StatusCode TestkitClient::Add(int32_t lhs, int32_t rhs, AddReply* reply) {
    AddRequest request{lhs, rhs};
    return asyncClient_.AddAsync(request).Wait(reply);
}

memrpc::StatusCode TestkitClient::Sleep(
    uint32_t delayMs,
    SleepReply* reply,
    memrpc::Priority priority,
    uint32_t execTimeoutMs) {
    SleepRequest request{delayMs};
    return asyncClient_.SleepAsync(request, priority, execTimeoutMs).Wait(reply);
}

void TestkitClient::Shutdown() {
    asyncClient_.Shutdown();
}

}  // namespace vpsdemo::testkit
