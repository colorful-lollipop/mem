#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_async_client.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_codec.h"
#include "testkit/testkit_protocol.h"
#include "testkit/testkit_service.h"

namespace VirusExecutorService::testkit {
namespace {

bool ThreadSanitizerEnabled()
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#endif
#endif
    return false;
}

constexpr MemRpc::StatusCode kExpectedEngineDeathStatus = MemRpc::StatusCode::CrashedDuringExecution;

void CloseHandles(MemRpc::BootstrapHandles* handles)
{
    if (handles == nullptr) {
        return;
    }
    int* fds[] = {
        &handles->shmFd,
        &handles->highReqEventFd,
        &handles->normalReqEventFd,
        &handles->respEventFd,
        &handles->reqCreditEventFd,
        &handles->respCreditEventFd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}

void RunTestkitServerProcess(MemRpc::BootstrapHandles handles)
{
    MemRpc::RpcServer server;
    server.SetBootstrapHandles(handles);
    TestkitServiceOptions options;
    options.enableFaultInjection = true;
    TestkitService service(options);
    RegisterHandlersToServer(&service, &server);
    if (server.Start() != MemRpc::StatusCode::Ok) {
        _exit(2);
    }
    server.Run();
    _exit(0);
}

std::shared_ptr<MemRpc::DevBootstrapChannel> CreateBootstrap()
{
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(&handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitClientTest, SyncAndAsyncCallsRoundTrip)
{
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    RegisterHandlersToServer(&service, &server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), MemRpc::StatusCode::Ok);

    EchoRequest echoRequest;
    echoRequest.text = "ping";
    auto future = asyncClient.EchoAsync(echoRequest);
    EchoReply echoReply;
    ASSERT_EQ(std::move(future).Wait(&echoReply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(echoReply.text, "ping");

    asyncClient.Shutdown();

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    AddReply addReply;
    EXPECT_EQ(client.Add(4, 5, &addReply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(addReply.sum, 9);

    client.Shutdown();
    server.Stop();
}

TEST(TestkitClientTest, HighPrioritySleepCompletesBeforeNormalBacklog)
{
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    MemRpc::ServerOptions options;
    options.highWorkerThreads = 1;
    options.normalWorkerThreads = 1;
    server.SetOptions(options);
    TestkitService service;
    RegisterHandlersToServer(&service, &server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), MemRpc::StatusCode::Ok);

    SleepRequest slowRequest;
    slowRequest.delayMs = 200;
    auto slowFuture = asyncClient.SleepAsync(slowRequest, MemRpc::Priority::Normal, 1000);

    SleepRequest fastRequest;
    fastRequest.delayMs = 5;
    SleepReply fastReply;
    EXPECT_EQ(asyncClient.SleepAsync(fastRequest, MemRpc::Priority::High, 1000).Wait(&fastReply),
              MemRpc::StatusCode::Ok);

    SleepReply slowReply;
    EXPECT_EQ(std::move(slowFuture).Wait(&slowReply), MemRpc::StatusCode::Ok);

    asyncClient.Shutdown();
    server.Stop();
}

TEST(TestkitClientTest, ProcessExitDuringHandlingFailsPendingAndRecoversAfterRestart)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based recovery test is unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();

    const pid_t firstChild = fork();
    ASSERT_GE(firstChild, 0);
    if (firstChild == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    SleepRequest sleepRequest;
    sleepRequest.delayMs = 1000;
    std::vector<uint8_t> sleepPayload;
    ASSERT_TRUE(MemRpc::EncodeMessage(sleepRequest, &sleepPayload));

    MemRpc::RpcCall sleepCall;
    sleepCall.opcode = static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep);
    sleepCall.execTimeoutMs = 5000;
    sleepCall.payload = sleepPayload;
    auto sleepFuture = client.InvokeAsync(sleepCall);

    MemRpc::RpcCall crashCall;
    crashCall.opcode = static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest);
    crashCall.priority = MemRpc::Priority::High;
    auto crashFuture = client.InvokeAsync(crashCall);

    int firstStatus = 0;
    ASSERT_EQ(waitpid(firstChild, &firstStatus, 0), firstChild);
    ASSERT_TRUE(WIFEXITED(firstStatus));
    EXPECT_EQ(WEXITSTATUS(firstStatus), 99);

    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply sleepReply;
    EXPECT_EQ(std::move(sleepFuture).Wait(&sleepReply), kExpectedEngineDeathStatus);
    MemRpc::RpcReply crashReply;
    EXPECT_EQ(std::move(crashFuture).Wait(&crashReply), kExpectedEngineDeathStatus);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(&handles);

    const pid_t secondChild = fork();
    ASSERT_GE(secondChild, 0);
    if (secondChild == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }

    EchoRequest echoRequest;
    echoRequest.text = "after-restart";
    std::vector<uint8_t> echoPayload;
    ASSERT_TRUE(MemRpc::EncodeMessage(echoRequest, &echoPayload));

    MemRpc::RpcCall echoCall;
    echoCall.opcode = static_cast<MemRpc::Opcode>(TestkitOpcode::Echo);
    echoCall.payload = echoPayload;

    MemRpc::RpcReply echoRpcReply;
    ASSERT_EQ(client.InvokeAsync(echoCall).Wait(&echoRpcReply), MemRpc::StatusCode::Ok);
    EchoReply echoReply;
    ASSERT_TRUE(MemRpc::DecodeMessage(echoRpcReply.payload, &echoReply));
    EXPECT_EQ(echoReply.text, "after-restart");

    client.Shutdown();
    kill(secondChild, SIGTERM);
    waitpid(secondChild, nullptr, 0);
}

}  // namespace VirusExecutorService::testkit
