#include "testkit/testkit_client.h"

#include <utility>

#include "virus_protection_service_log.h"

namespace VirusExecutorService::testkit {

TestkitClient::TestkitClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
    : asyncClient_(std::move(bootstrap)) {}

TestkitClient::~TestkitClient() = default;

void TestkitClient::SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap) {
    asyncClient_.SetBootstrapChannel(std::move(bootstrap));
}

MemRpc::StatusCode TestkitClient::Init() {
    const MemRpc::StatusCode status = asyncClient_.Init();
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("TestkitClient::Init failed status=%{public}d", static_cast<int>(status));
    }
    return status;
}

MemRpc::StatusCode TestkitClient::Echo(const std::string& text, EchoReply* reply) {
    if (reply == nullptr) {
        HILOGE("TestkitClient::Echo failed: reply is null");
        return MemRpc::StatusCode::InvalidArgument;
    }
    EchoRequest request{text};
    const MemRpc::StatusCode status = asyncClient_.EchoAsync(request).Wait(reply);
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("TestkitClient::Echo failed status=%{public}d", static_cast<int>(status));
    }
    return status;
}

MemRpc::StatusCode TestkitClient::Add(int32_t lhs, int32_t rhs, AddReply* reply) {
    if (reply == nullptr) {
        HILOGE("TestkitClient::Add failed: reply is null");
        return MemRpc::StatusCode::InvalidArgument;
    }
    AddRequest request{lhs, rhs};
    const MemRpc::StatusCode status = asyncClient_.AddAsync(request).Wait(reply);
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("TestkitClient::Add failed status=%{public}d", static_cast<int>(status));
    }
    return status;
}

MemRpc::StatusCode TestkitClient::Sleep(
    uint32_t delayMs,
    SleepReply* reply,
    MemRpc::Priority priority,
    uint32_t execTimeoutMs) {
    if (reply == nullptr) {
        HILOGE("TestkitClient::Sleep failed: reply is null");
        return MemRpc::StatusCode::InvalidArgument;
    }
    SleepRequest request{delayMs};
    const MemRpc::StatusCode status =
        asyncClient_.SleepAsync(request, priority, execTimeoutMs).Wait(reply);
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("TestkitClient::Sleep failed status=%{public}d delay_ms=%{public}u",
            static_cast<int>(status), delayMs);
    }
    return status;
}

void TestkitClient::Shutdown() {
    asyncClient_.Shutdown();
}

}  // namespace VirusExecutorService::testkit
