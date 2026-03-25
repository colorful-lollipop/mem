#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_

#include <memory>

#include "memrpc/client/typed_future.h"
#include "testkit/testkit_codec.h"

namespace VirusExecutorService::testkit {

class TestkitAsyncClient {
public:
    explicit TestkitAsyncClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap = nullptr);
    ~TestkitAsyncClient();

    TestkitAsyncClient(const TestkitAsyncClient&) = delete;
    TestkitAsyncClient& operator=(const TestkitAsyncClient&) = delete;
    TestkitAsyncClient(TestkitAsyncClient&&) noexcept = default;
    TestkitAsyncClient& operator=(TestkitAsyncClient&&) noexcept = default;

    void SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap);
    MemRpc::StatusCode Init();
    MemRpc::TypedFuture<EchoReply> EchoAsync(const EchoRequest& request);
    MemRpc::TypedFuture<AddReply> AddAsync(const AddRequest& request);
    MemRpc::TypedFuture<SleepReply> SleepAsync(const SleepRequest& request,
                                               MemRpc::Priority priority = MemRpc::Priority::Normal,
                                               uint32_t execTimeoutMs = 30000);
    void Shutdown();

private:
    MemRpc::RpcClient client_;
};

}  // namespace VirusExecutorService::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_
