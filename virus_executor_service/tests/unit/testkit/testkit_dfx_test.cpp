#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_codec.h"
#include "testkit/testkit_failure_monitor.h"
#include "testkit/testkit_protocol.h"
#include "testkit_resilient_invoker.h"
#include "testkit/testkit_service.h"

namespace VirusExecutorService::testkit {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles) {
    if (handles.shmFd >= 0) close(handles.shmFd);
    if (handles.highReqEventFd >= 0) close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0) close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0) close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0) close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0) close(handles.respCreditEventFd);
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

pid_t ForkServer(const std::shared_ptr<MemRpc::DevBootstrapChannel>& bootstrap) {
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

MemRpc::RpcCall MakeAddCall(int32_t lhs, int32_t rhs) {
    AddRequest request;
    request.lhs = lhs;
    request.rhs = rhs;
    std::vector<uint8_t> payload;
    MemRpc::EncodeMessage(request, &payload);
    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(TestkitOpcode::Add);
    call.payload = std::move(payload);
    return call;
}

MemRpc::RpcCall MakeSleepCall(uint32_t delayMs) {
    SleepRequest request;
    request.delayMs = delayMs;
    std::vector<uint8_t> payload;
    MemRpc::EncodeMessage(request, &payload);
    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(TestkitOpcode::Sleep);
    call.execTimeoutMs = 5000;
    call.payload = std::move(payload);
    return call;
}

MemRpc::RpcCall MakeFaultCall(MemRpc::Opcode opcode) {
    MemRpc::RpcCall call;
    call.opcode = opcode;
    call.priority = MemRpc::Priority::High;
    return call;
}

std::shared_ptr<MemRpc::DevBootstrapChannel> CreateBootstrap() {
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles{};
    EXPECT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitDfxTest, CrashDuringBatchTracksAllFailures) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 10; ++i) {
        batch.push_back(MakeAddCall(i, i + 1));
    }
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));

    invoker.SubmitBatch(batch);

    int status = 0;
    waitpid(child, &status, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    EXPECT_GT(failed.size(), 0u);
    EXPECT_EQ(completed.size() + failed.size(), batch.size());

    for (const auto& failedCall : failed) {
        EXPECT_EQ(failedCall.failureStatus, MemRpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, ReplayPolicyResubmitsAddCallsAfterCrash) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto replayNonFault = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)
                   ? ReplayDecision::Skip
                   : ReplayDecision::Replay;
    };
    ResilientBatchInvoker invoker(bootstrap, replayNonFault);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 5; ++i) {
        batch.push_back(MakeAddCall(i, 100));
    }
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    ASSERT_TRUE(completed.empty());
    ASSERT_GT(invoker.GetFailedCalls().size(), 0u);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    ASSERT_EQ(replayed.size(), 5u);

    std::vector<MemRpc::RpcReply> replayCompleted;
    invoker.CollectResults(&replayCompleted);
    ASSERT_EQ(replayCompleted.size(), 5u);
    EXPECT_EQ(invoker.GetFailedCalls().size(), 1u);
    if (!invoker.GetFailedCalls().empty()) {
        EXPECT_EQ(
            invoker.GetFailedCalls()[0].opcode,
            static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest));
    }

    for (int i = 0; i < 5; ++i) {
        const auto& reply = replayCompleted[static_cast<size_t>(i)];
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
        AddReply decoded;
        ASSERT_TRUE(MemRpc::DecodeMessage(reply.payload, &decoded));
        EXPECT_EQ(decoded.sum, i + 100);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, ReplayPolicyResubmitsInFlightCallsAfterCrash) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto replayNonFault = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)
                   ? ReplayDecision::Skip
                   : ReplayDecision::Replay;
    };
    ResilientBatchInvoker invoker(bootstrap, replayNonFault);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 5; ++i) {
        batch.push_back(MakeSleepCall(200));
    }
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    ASSERT_GT(invoker.GetFailedCalls().size(), 0u);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    EXPECT_GT(replayed.size(), 0u);

    std::vector<MemRpc::RpcReply> replayCompleted;
    invoker.CollectResults(&replayCompleted);
    EXPECT_EQ(invoker.GetFailedCalls().size(), 1u);
    if (!invoker.GetFailedCalls().empty()) {
        EXPECT_EQ(
            invoker.GetFailedCalls()[0].opcode,
            static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest));
    }

    for (const auto& reply : replayCompleted) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, ReplayPolicySkipsSelectedCalls) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto skipAdd = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::Add)
                   ? ReplayDecision::Skip
                   : ReplayDecision::Replay;
    };

    ResilientBatchInvoker invoker(bootstrap, skipAdd);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    batch.push_back(MakeAddCall(1, 2));
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    const size_t totalFailed = invoker.GetFailedCalls().size();
    ASSERT_GT(totalFailed, 0u);

    size_t addFailures = 0;
    for (const auto& failedCall : invoker.GetFailedCalls()) {
        if (failedCall.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::Add)) {
            ++addFailures;
        }
    }

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
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

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto hangFuture = client.InvokeAsync(
        MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::HangForTest)));

    usleep(100000);
    KillAndReap(child);
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply hangReply;
    EXPECT_EQ(hangFuture.Wait(&hangReply), MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(10, 20);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 30);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, OomKilledChildRecovery) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto oomFuture = client.InvokeAsync(
        MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::OomForTest)));

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

    MemRpc::RpcReply oomReply;
    EXPECT_EQ(oomFuture.Wait(&oomReply), MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(7, 8);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 15);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, StackOverflowChildRecovery) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = client.InvokeAsync(
        MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::StackOverflowForTest)));

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

    MemRpc::RpcReply reply;
    EXPECT_EQ(future.Wait(&reply), MemRpc::StatusCode::PeerDisconnected);

    MemRpc::BootstrapHandles handles{};
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(3, 4);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 7);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, BatchPartialCompletionTracking) {
    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 20; ++i) {
        batch.push_back(MakeAddCall(i, 1));
    }
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    EXPECT_EQ(completed.size() + failed.size(), batch.size());
    EXPECT_GT(failed.size(), 0u);

    for (const auto& reply : completed) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }
    for (const auto& failedCall : failed) {
        EXPECT_EQ(failedCall.failureStatus, MemRpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, MultipleConsecutiveCrashesAndRecoveries) {
    auto bootstrap = CreateBootstrap();

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    for (int cycle = 0; cycle < 3; ++cycle) {
        pid_t child = ForkServer(bootstrap);
        ASSERT_GT(child, 0) << "cycle " << cycle;

        const auto addCall = MakeAddCall(cycle, 10);
        MemRpc::RpcReply addReply;
        ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), MemRpc::StatusCode::Ok)
            << "cycle " << cycle;
        AddReply decoded;
        ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
        EXPECT_EQ(decoded.sum, cycle + 10);

        auto crashFuture = client.InvokeAsync(
            MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
        waitpid(child, nullptr, 0);
        bootstrap->SimulateEngineDeathForTest();

        MemRpc::RpcReply crashReply;
        EXPECT_EQ(crashFuture.Wait(&crashReply), MemRpc::StatusCode::PeerDisconnected)
            << "cycle " << cycle;

        MemRpc::BootstrapHandles handles{};
        ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok) << "cycle " << cycle;
        CloseHandles(handles);
    }

    pid_t finalChild = ForkServer(bootstrap);
    ASSERT_GT(finalChild, 0);

    const auto addCall = MakeAddCall(99, 1);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).WaitAndTake(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
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

    MemRpc::RpcFailure failure;
    failure.status = MemRpc::StatusCode::ExecTimeout;

    monitor.OnFailure(failure);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 0);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 1);
}

}  // namespace VirusExecutorService::testkit
