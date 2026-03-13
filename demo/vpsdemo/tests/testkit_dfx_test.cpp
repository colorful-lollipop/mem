#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "testkit_codec.h"
#include "testkit_failure_monitor.h"
#include "testkit_protocol.h"
#include "testkit_resilient_invoker.h"
#include "testkit_service.h"

namespace vpsdemo::testkit {
namespace {

void CloseHandles(memrpc::BootstrapHandles& handles) {
    if (handles.shmFd >= 0) close(handles.shmFd);
    if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0) close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
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

pid_t ForkServer(const std::shared_ptr<memrpc::PosixDemoBootstrapChannel>& bootstrap) {
    const pid_t child = fork();
    if (child == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }
    return child;
}

void KillAndReap(pid_t child) {
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

memrpc::RpcCall MakeAddCall(int32_t lhs, int32_t rhs) {
    AddRequest request;
    request.lhs = lhs;
    request.rhs = rhs;
    std::vector<uint8_t> payload;
    memrpc::EncodeMessage(request, &payload);
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(TestkitOpcode::Add);
    call.payload = std::move(payload);
    return call;
}

memrpc::RpcCall MakeSleepCall(uint32_t delayMs) {
    SleepRequest request;
    request.delayMs = delayMs;
    std::vector<uint8_t> payload;
    memrpc::EncodeMessage(request, &payload);
    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(TestkitOpcode::Sleep);
    call.execTimeoutMs = 5000;
    call.payload = std::move(payload);
    return call;
}

memrpc::RpcCall MakeFaultCall(memrpc::Opcode opcode) {
    memrpc::RpcCall call;
    call.opcode = opcode;
    call.priority = memrpc::Priority::High;
    return call;
}

std::shared_ptr<memrpc::PosixDemoBootstrapChannel> CreateBootstrap() {
    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles handles{};
    EXPECT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitDfxTest, CrashDuringBatchTracksAllFailures) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), memrpc::StatusCode::Ok);

    std::vector<memrpc::RpcCall> batch;
    for (int i = 0; i < 10; ++i) {
        batch.push_back(MakeAddCall(i, i + 1));
    }
    batch.push_back(MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)));

    invoker.SubmitBatch(batch);

    int status = 0;
    waitpid(child, &status, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<memrpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    EXPECT_GT(failed.size(), 0u);
    EXPECT_EQ(completed.size() + failed.size(), batch.size());

    for (const auto& failedCall : failed) {
        EXPECT_EQ(failedCall.failureStatus, memrpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, ReplayPolicyResubmitsAfterCrash) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto replayNonFault = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)
                   ? ReplayDecision::Skip
                   : ReplayDecision::Replay;
    };
    ResilientBatchInvoker invoker(bootstrap, replayNonFault);
    ASSERT_EQ(invoker.Init(), memrpc::StatusCode::Ok);

    std::vector<memrpc::RpcCall> batch;
    for (int i = 0; i < 5; ++i) {
        batch.push_back(MakeAddCall(i, 100));
    }
    batch.push_back(MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<memrpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    ASSERT_GT(invoker.GetFailedCalls().size(), 0u);

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    EXPECT_GT(replayed.size(), 0u);

    std::vector<memrpc::RpcReply> replayCompleted;
    invoker.CollectResults(&replayCompleted);
    EXPECT_EQ(invoker.GetFailedCalls().size(), 1u);
    if (!invoker.GetFailedCalls().empty()) {
        EXPECT_EQ(
            invoker.GetFailedCalls()[0].opcode,
            static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest));
    }

    for (const auto& reply : replayCompleted) {
        EXPECT_EQ(reply.status, memrpc::StatusCode::Ok);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, ReplayPolicySkipsSelectedCalls) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto skipAdd = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<memrpc::Opcode>(TestkitOpcode::Add)
                   ? ReplayDecision::Skip
                   : ReplayDecision::Replay;
    };

    ResilientBatchInvoker invoker(bootstrap, skipAdd);
    ASSERT_EQ(invoker.Init(), memrpc::StatusCode::Ok);

    std::vector<memrpc::RpcCall> batch;
    batch.push_back(MakeAddCall(1, 2));
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<memrpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    const size_t totalFailed = invoker.GetFailedCalls().size();
    ASSERT_GT(totalFailed, 0u);

    size_t addFailures = 0;
    for (const auto& failedCall : invoker.GetFailedCalls()) {
        if (failedCall.opcode == static_cast<memrpc::Opcode>(TestkitOpcode::Add)) {
            ++addFailures;
        }
    }

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    EXPECT_EQ(replayed.size(), totalFailed - addFailures);
    EXPECT_EQ(invoker.GetFailedCalls().size(), addFailures);

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, HangingChildKilledAndRecovered) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    memrpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    auto hangFuture = client.InvokeAsync(
        MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::HangForTest)));

    usleep(100000);
    KillAndReap(child);
    bootstrap->SimulateEngineDeathForTest();

    memrpc::RpcReply hangReply;
    EXPECT_EQ(hangFuture.Wait(&hangReply), memrpc::StatusCode::PeerDisconnected);

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(10, 20);
    memrpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), memrpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(memrpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 30);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, OomKilledChildRecovery) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    memrpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    auto oomFuture = client.InvokeAsync(
        MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::OomForTest)));

    int status = 0;
    int waitResult = 0;
    for (int i = 0; i < 30; ++i) {
        waitResult = waitpid(child, &status, WNOHANG);
        if (waitResult > 0) break;
        usleep(100000);
    }
    if (waitResult == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    memrpc::RpcReply oomReply;
    EXPECT_EQ(oomFuture.Wait(&oomReply), memrpc::StatusCode::PeerDisconnected);

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(7, 8);
    memrpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), memrpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(memrpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 15);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, StackOverflowChildRecovery) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    memrpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    auto future = client.InvokeAsync(
        MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::StackOverflowForTest)));

    int status = 0;
    int waitResult = 0;
    for (int i = 0; i < 30; ++i) {
        waitResult = waitpid(child, &status, WNOHANG);
        if (waitResult > 0) break;
        usleep(100000);
    }
    if (waitResult == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    memrpc::RpcReply reply;
    EXPECT_EQ(future.Wait(&reply), memrpc::StatusCode::PeerDisconnected);

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(3, 4);
    memrpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), memrpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(memrpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 7);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, BatchPartialCompletionTracking) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), memrpc::StatusCode::Ok);

    std::vector<memrpc::RpcCall> batch;
    for (int i = 0; i < 20; ++i) {
        batch.push_back(MakeAddCall(i, 1));
    }
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<memrpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    EXPECT_EQ(completed.size() + failed.size(), batch.size());
    EXPECT_GT(failed.size(), 0u);

    for (const auto& reply : completed) {
        EXPECT_EQ(reply.status, memrpc::StatusCode::Ok);
    }
    for (const auto& failedCall : failed) {
        EXPECT_EQ(failedCall.failureStatus, memrpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, MultipleConsecutiveCrashesAndRecoveries) {
    auto bootstrap = CreateBootstrap();

    memrpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    for (int cycle = 0; cycle < 3; ++cycle) {
        pid_t child = ForkServer(bootstrap);
        ASSERT_GT(child, 0) << "cycle " << cycle;

        const auto addCall = MakeAddCall(cycle, 10);
        memrpc::RpcReply addReply;
        ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), memrpc::StatusCode::Ok)
            << "cycle " << cycle;
        AddReply decoded;
        ASSERT_TRUE(memrpc::DecodeMessage(addReply.payload, &decoded));
        EXPECT_EQ(decoded.sum, cycle + 10);

        auto crashFuture = client.InvokeAsync(
            MakeFaultCall(static_cast<memrpc::Opcode>(TestkitOpcode::CrashForTest)));
        waitpid(child, nullptr, 0);
        bootstrap->SimulateEngineDeathForTest();

        memrpc::RpcReply crashReply;
        EXPECT_EQ(crashFuture.Wait(&crashReply), memrpc::StatusCode::PeerDisconnected)
            << "cycle " << cycle;

        memrpc::BootstrapHandles handles{};
        ASSERT_EQ(bootstrap->OpenSession(handles), memrpc::StatusCode::Ok) << "cycle " << cycle;
        CloseHandles(handles);
    }

    pid_t finalChild = ForkServer(bootstrap);
    ASSERT_GT(finalChild, 0);

    const auto addCall = MakeAddCall(99, 1);
    memrpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), memrpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(memrpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 100);

    client.Shutdown();
    KillAndReap(finalChild);
}

TEST(TestkitDfxTest, FailureMonitorTriggersAfterExecTimeoutThreshold) {
    int triggered = 0;
    FailureMonitor::Options options;
    options.windowMs = 60000;
    options.execTimeoutThreshold = 3;

    FailureMonitor monitor(options, [&] { ++triggered; });

    memrpc::RpcFailure failure;
    failure.status = memrpc::StatusCode::ExecTimeout;

    monitor.OnFailure(failure);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 0);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 1);
}

}  // namespace vpsdemo::testkit
