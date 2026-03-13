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
#include "testkit/testkit_async_client.h"
#include "testkit/testkit_client.h"
#include "testkit/testkit_codec.h"
#include "testkit/testkit_protocol.h"
#include "testkit/testkit_service.h"

namespace virus_executor_service::testkit {
namespace {

void CloseHandles(MemRpc::BootstrapHandles* handles) {
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

void RunTestkitServerProcess(MemRpc::BootstrapHandles handles) {
    MemRpc::RpcServer server;
    server.SetBootstrapHandles(handles);
    TestkitService service({.enableFaultInjection = true});
    service.RegisterHandlers(&server);
    if (server.Start() != MemRpc::StatusCode::Ok) {
        _exit(2);
    }
    server.Run();
    _exit(0);
}

std::shared_ptr<MemRpc::PosixDemoBootstrapChannel> CreateBootstrap() {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    MemRpc::BootstrapHandles handles{};
    EXPECT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(&handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitClientTest, SyncAndAsyncCallsRoundTrip) {
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), MemRpc::StatusCode::Ok);

    EchoRequest echoRequest;
    echoRequest.text = "ping";
    auto future = asyncClient.EchoAsync(echoRequest);
    EchoReply echoReply;
    ASSERT_EQ(future.Wait(&echoReply), MemRpc::StatusCode::Ok);
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

TEST(TestkitClientTest, HighPrioritySleepCompletesBeforeNormalBacklog) {
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    server.SetOptions({.highWorkerThreads = 1, .normalWorkerThreads = 1});
    TestkitService service;
    service.RegisterHandlers(&server);
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
    EXPECT_EQ(slowFuture.Wait(&slowReply), MemRpc::StatusCode::Ok);

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
    EXPECT_EQ(sleepFuture.Wait(&sleepReply), MemRpc::StatusCode::PeerDisconnected);
    MemRpc::RpcReply crashReply;
    EXPECT_EQ(crashFuture.Wait(&crashReply), MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles handles{};
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
    ASSERT_EQ(client.InvokeAsync(echoCall).WaitAndTake(&echoRpcReply), MemRpc::StatusCode::Ok);
    EchoReply echoReply;
    ASSERT_TRUE(MemRpc::DecodeMessage(echoRpcReply.payload, &echoReply));
    EXPECT_EQ(echoReply.text, "after-restart");

    client.Shutdown();
    kill(secondChild, SIGTERM);
    waitpid(secondChild, nullptr, 0);
}

TEST(TestkitClientTest, TypedThenDecodesReply) {
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    TestkitService service;
    service.RegisterHandlers(&server);
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    TestkitAsyncClient asyncClient(bootstrap);
    ASSERT_EQ(asyncClient.Init(), MemRpc::StatusCode::Ok);

    EchoRequest request;
    request.text = "typed-then";

    std::atomic<bool> called{false};
    std::mutex mutex;
    EchoReply received;

    auto future = asyncClient.EchoAsync(request);
    future.Then([&](MemRpc::StatusCode status, EchoReply reply) {
        EXPECT_EQ(status, MemRpc::StatusCode::Ok);
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
