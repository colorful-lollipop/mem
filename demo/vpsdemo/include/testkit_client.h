#ifndef VPSDEMO_TESTKIT_CLIENT_H_
#define VPSDEMO_TESTKIT_CLIENT_H_

#include <memory>
#include <string>

#include "testkit_async_client.h"

namespace vpsdemo::testkit {

class TestkitClient {
 public:
    explicit TestkitClient(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap = nullptr);
    ~TestkitClient();

    TestkitClient(const TestkitClient&) = delete;
    TestkitClient& operator=(const TestkitClient&) = delete;
    TestkitClient(TestkitClient&&) noexcept = default;
    TestkitClient& operator=(TestkitClient&&) noexcept = default;

    void SetBootstrapChannel(std::shared_ptr<memrpc::IBootstrapChannel> bootstrap);
    memrpc::StatusCode Init();
    memrpc::StatusCode Echo(const std::string& text, EchoReply* reply);
    memrpc::StatusCode Add(int32_t lhs, int32_t rhs, AddReply* reply);
    memrpc::StatusCode Sleep(
        uint32_t delayMs,
        SleepReply* reply,
        memrpc::Priority priority = memrpc::Priority::Normal,
        uint32_t execTimeoutMs = 30000);
    void Shutdown();

 private:
    TestkitAsyncClient asyncClient_;
};

}  // namespace vpsdemo::testkit

#endif  // VPSDEMO_TESTKIT_CLIENT_H_
