#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/typed_invoker.h"
#include "memrpc/server/rpc_server.h"
#include "virus_executor_service/testkit/testkit_async_client.h"
#include "virus_executor_service/testkit/testkit_client.h"
#include "virus_executor_service/testkit/testkit_codec.h"
#include "virus_executor_service/testkit/testkit_protocol.h"
#include "virus_executor_service/testkit/testkit_service.h"

namespace virus_executor_service::testkit {
namespace {

void CloseHandles(memrpc::BootstrapHandles* handles) {
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

void RunTestkitServerProcess(memrpc::BootstrapHandles handles) {
    memrpc::RpcServer server;
    server.SetBootstrapHandles(handles);
    TestkitService service({.enableFaultInjection = true});
    service.RegisterHandlers(&server);
    if (server.Start() != memrpc::StatusCode::Ok) {
        _exit(2);
    }
    server.Run();
    _exit(0);
}

std::shared_ptr<memrpc::PosixDemoBootstrapChannel> CreateBootstrap() {
    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles handles{};
    EXPECT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(&handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitClientTest, SyncAndAsyncCallsRoundTrip) {
    auto bootstrap = CreateBootstrap();

    memrpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), memrpc::StatusCode::Ok);

    EchoRequest echoRequest;
    echoRequest.text = "ping";
    auto future = asyncClient.EchoAsync(echoRequest);
    EchoReply echoReply;
    ASSERT_EQ(future.Wait(&echoReply), memrpc::StatusCode::Ok);
    EXPECT_EQ(echoReply.text, "ping");

    asyncClient.Shutdown();

    TestkitClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    AddReply addReply;
    EXPECT_EQ(client.Add(4, 5, &addReply), memrpc::StatusCode::Ok);
    EXPECT_EQ(addReply.sum, 9);

    client.Shutdown();
    server.Stop();
}

TEST(TestkitClientTest, HighPrioritySleepCompletesBeforeNormalBacklog) {
    auto bootstrap = CreateBootstrap();

    memrpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.SetOptions({.highWorkerThreads = 1, .normalWorkerThreads = 1});
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), memrpc::StatusCode::Ok);

    SleepRequest slowRequest;
    slowRequest.delayMs = 200;
    auto slowFuture = asyncClient.SleepAsync(slowRequest, memrpc::Priority::Normal, 1000);

    SleepRequest fastRequest;
    fastRequest.delayMs = 5;
    SleepReply fastReply;
    EXPECT_EQ(asyncClient.SleepAsync(fastRequest, memrpc::Priority::High, 1000).Wait(&fastReply),
              memrpc::StatusCode::Ok);

    SleepReply slowReply;
    EXPECT_EQ(slowFuture.Wait(&slowReply), memrpc::StatusCode::Ok);

    asyncClient.Shutdown();
    server.Stop();
}

TEST(TestkitClientTest, ProcessExitDuringHandlingFailsPendingAndRecoversAfterRestart) {
    auto bootstrap = CreateBootstrap();

    const pid_t firstChild = fork();
    ASSERT_GE(firstChild, 0);
    if (firstChild == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }

    memrpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    SleepRequest sleepRequest;
    sleepRequest.delayMs = 1000;
    std::vector<uint8_t> sleepPayload;
    ASSERT_TRUE(memrpc::EncodeMessage(sleepRequest, &sleepPayload));

    memrpc::RpcCall sleepCall;
    sleepCall.opcode = static_cast<memrpc::Opcode>(TestkitOpcode::Sleep);
    sleepCall.execTimeoutMs = 5000;
    sleepCall.payload = sleepPayload;
    auto sleepFuture = client.InvokeAsync(sleepCall);

    memrpc::RpcCall crashCall;
    crashCall.opcode = static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest);
    crashCall.priority = memrpc::Priority::High;
    auto crashFuture = client.InvokeAsync(crashCall);

    int firstStatus = 0;
    ASSERT_EQ(waitpid(firstChild, &firstStatus, 0), firstChild);
    ASSERT_TRUE(WIFEXITED(firstStatus));
    EXPECT_EQ(WEXITSTATUS(firstStatus), 99);

    bootstrap->SimulateEngineDeathForTest();

    memrpc::RpcReply sleepReply;
    EXPECT_EQ(sleepFuture.Wait(&sleepReply), memrpc::StatusCode::PeerDisconnected);
    memrpc::RpcReply crashReply;
    EXPECT_EQ(crashFuture.Wait(&crashReply), memrpc::StatusCode::PeerDisconnected);

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(&handles);

    const pid_t secondChild = fork();
    ASSERT_GE(secondChild, 0);
    if (secondChild == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }

    EchoRequest echoRequest;
    echoRequest.text = "after-restart";
    std::vector<uint8_t> echoPayload;
    ASSERT_TRUE(memrpc::EncodeMessage(echoRequest, &echoPayload));

    memrpc::RpcCall echoCall;
    echoCall.opcode = static_cast<memrpc::Opcode>(TestkitOpcode::Echo);
    echoCall.payload = echoPayload;

    memrpc::RpcReply echoRpcReply;
    ASSERT_EQ(client.InvokeAsync(echoCall).WaitAndTake(&echoRpcReply), memrpc::StatusCode::Ok);
    EchoReply echoReply;
    ASSERT_TRUE(memrpc::DecodeMessage(echoRpcReply.payload, &echoReply));
    EXPECT_EQ(echoReply.text, "after-restart");

    client.Shutdown();
    kill(secondChild, SIGTERM);
    waitpid(secondChild, nullptr, 0);
}

TEST(TestkitClientTest, TypedThenDecodesReply) {
    auto bootstrap = CreateBootstrap();

    memrpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), memrpc::StatusCode::Ok);

    EchoRequest request;
    request.text = "typed-then";

    std::atomic<bool> called{false};
    std::mutex mutex;
    EchoReply received;

    auto future = asyncClient.EchoAsync(request);
    future.Then([&](memrpc::StatusCode status, EchoReply reply) {
        EXPECT_EQ(status, memrpc::StatusCode::Ok);
        std::lock_guard<std::mutex> lock(mutex);
        received = std::move(reply);
        called.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!called.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(called.load());
    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(received.text, "typed-then");
    }

    asyncClient.Shutdown();
    server.Stop();
}

}  // namespace virus_executor_service::testkit
