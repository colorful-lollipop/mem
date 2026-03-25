#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CLIENT_H_

#include <memory>
#include <string>

#include "testkit/testkit_async_client.h"

namespace VirusExecutorService::testkit {

class TestkitClient {
public:
    explicit TestkitClient(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap = nullptr);
    ~TestkitClient();

    TestkitClient(const TestkitClient&) = delete;
    TestkitClient& operator=(const TestkitClient&) = delete;
    TestkitClient(TestkitClient&&) noexcept = default;
    TestkitClient& operator=(TestkitClient&&) noexcept = default;

    void SetBootstrapChannel(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap);
    MemRpc::StatusCode Init();
    MemRpc::StatusCode Echo(const std::string& text, EchoReply* reply);
    MemRpc::StatusCode Add(int32_t lhs, int32_t rhs, AddReply* reply);
    MemRpc::StatusCode Sleep(uint32_t delayMs,
                             SleepReply* reply,
                             MemRpc::Priority priority = MemRpc::Priority::Normal,
                             uint32_t execTimeoutMs = 30000);
    void Shutdown();

private:
    TestkitAsyncClient asyncClient_;
};

}  // namespace VirusExecutorService::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CLIENT_H_
