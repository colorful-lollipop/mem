#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_

#include <memory>

#include "memrpc/client/typed_future.h"
#include "virus_executor_service/testkit/testkit_codec.h"

namespace virus_executor_service::testkit {

class TestkitAsyncClient {
 public:
    explicit TestkitAsyncClient(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap = nullptr);
    ~TestkitAsyncClient();

    TestkitAsyncClient(const TestkitAsyncClient&) = delete;
    TestkitAsyncClient& operator=(const TestkitAsyncClient&) = delete;
    TestkitAsyncClient(TestkitAsyncClient&&) noexcept = default;
    TestkitAsyncClient& operator=(TestkitAsyncClient&&) noexcept = default;

    void SetBootstrapChannel(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap);
    memrpc::StatusCode Init();
    memrpc::TypedFuture<EchoReply> EchoAsync(const EchoRequest& request);
    memrpc::TypedFuture<AddReply> AddAsync(const AddRequest& request);
    memrpc::TypedFuture<SleepReply> SleepAsync(
        const SleepRequest& request,
        memrpc::Priority priority = memrpc::Priority::Normal,
        uint32_t execTimeoutMs = 30000);
    void Shutdown();

 private:
    memrpc::RpcClient client_;
};

}  // namespace virus_executor_service::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_ASYNC_CLIENT_H_
