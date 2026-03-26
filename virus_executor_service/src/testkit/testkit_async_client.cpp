#include "testkit/testkit_async_client.h"

#include <utility>
#include <vector>

#include "testkit/testkit_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService::testkit {
namespace {

template <typename Request, typename Reply>
MemRpc::TypedFuture<Reply> InvokeTypedAsync(MemRpc::RpcClient* client,
                                            MemRpc::Opcode opcode,
                                            const Request& request,
                                            MemRpc::Priority priority = MemRpc::Priority::Normal,
                                            uint32_t execTimeoutMs = 30000)
{
    if (client == nullptr) {
        return {};
    }

    std::vector<uint8_t> payload;
    if (!MemRpc::EncodeMessage<Request>(request, &payload)) {
        return {};
    }

    MemRpc::RpcCall call;
    call.opcode = opcode;
    call.priority = priority;
    call.execTimeoutMs = execTimeoutMs;
    call.payload = std::move(payload);
    return MemRpc::TypedFuture<Reply>(client->InvokeAsync(std::move(call)));
}

}  // namespace

TestkitAsyncClient::TestkitAsyncClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
    : client_(std::move(bootstrap))
{
}

TestkitAsyncClient::~TestkitAsyncClient() = default;

void TestkitAsyncClient::SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap)
{
    client_.SetBootstrapChannel(std::move(bootstrap));
}

MemRpc::StatusCode TestkitAsyncClient::Init()
{
    const MemRpc::StatusCode status = client_.Init();
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("TestkitAsyncClient::Init failed status=%{public}d", static_cast<int>(status));
    }
    return status;
}

MemRpc::TypedFuture<EchoReply> TestkitAsyncClient::EchoAsync(const EchoRequest& request)
{
    return InvokeTypedAsync<EchoRequest, EchoReply>(&client_,
                                                    static_cast<MemRpc::Opcode>(TestkitOpcode::Echo),
                                                    request);
}

MemRpc::TypedFuture<AddReply> TestkitAsyncClient::AddAsync(const AddRequest& request)
{
    return InvokeTypedAsync<AddRequest, AddReply>(&client_,
                                                  static_cast<MemRpc::Opcode>(TestkitOpcode::Add),
                                                  request);
}

MemRpc::TypedFuture<SleepReply> TestkitAsyncClient::SleepAsync(const SleepRequest& request,
                                                               MemRpc::Priority priority,
                                                               uint32_t execTimeoutMs)
{
    return InvokeTypedAsync<SleepRequest, SleepReply>(&client_,
                                                      static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep),
                                                      request,
                                                      priority,
                                                      execTimeoutMs);
}

void TestkitAsyncClient::Shutdown()
{
    client_.Shutdown();
}

}  // namespace VirusExecutorService::testkit
