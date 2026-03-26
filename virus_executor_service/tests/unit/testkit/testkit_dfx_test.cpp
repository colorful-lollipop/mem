#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <memory>
#include <utility>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "testkit/testkit_codec.h"
#include "testkit/testkit_failure_monitor.h"
#include "testkit/testkit_protocol.h"
#include "testkit/testkit_service.h"
#include "testkit_resilient_invoker.h"

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

void CloseHandles(MemRpc::BootstrapHandles& handles)
{
    if (handles.shmFd >= 0)
        close(handles.shmFd);
    if (handles.highReqEventFd >= 0)
        close(handles.highReqEventFd);
    if (handles.normalReqEventFd >= 0)
        close(handles.normalReqEventFd);
    if (handles.respEventFd >= 0)
        close(handles.respEventFd);
    if (handles.reqCreditEventFd >= 0)
        close(handles.reqCreditEventFd);
    if (handles.respCreditEventFd >= 0)
        close(handles.respCreditEventFd);
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

pid_t ForkServer(const std::shared_ptr<MemRpc::DevBootstrapChannel>& bootstrap)
{
    const pid_t child = fork();
    if (child == 0) {
        RunTestkitServerProcess(bootstrap->serverHandles());
    }
    return child;
}

void KillAndReap(pid_t child)
{
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

MemRpc::RpcCall MakeAddCall(int32_t lhs, int32_t rhs)
{
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

MemRpc::RpcCall MakeSleepCall(uint32_t delayMs)
{
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

MemRpc::RpcCall MakeFaultCall(MemRpc::Opcode opcode)
{
    MemRpc::RpcCall call;
    call.opcode = opcode;
    call.priority = MemRpc::Priority::High;
    return call;
}

std::shared_ptr<MemRpc::DevBootstrapChannel> CreateBootstrap()
{
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    return bootstrap;
}

}  // namespace

TEST(TestkitDfxTest, CrashDuringBatchTracksAllFailures)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

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
        EXPECT_EQ(failedCall.failureStatus, kExpectedEngineDeathStatus);
        EXPECT_EQ(failedCall.runtimeSnapshot.lastTrigger, MemRpc::RecoveryTrigger::EngineDeath);
        EXPECT_TRUE(failedCall.hasRecoveryEvent);
        EXPECT_EQ(failedCall.recoveryEvent.trigger, MemRpc::RecoveryTrigger::EngineDeath);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, ReplayPolicyResubmitsAddCallsAfterCrash)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto replayNonFault = [](const FailedCallRecord& record) {
        EXPECT_TRUE(record.hasRecoveryEvent);
        EXPECT_EQ(record.runtimeSnapshot.lastTrigger, MemRpc::RecoveryTrigger::EngineDeath);
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest) ? ReplayDecision::Skip
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
    ASSERT_GT(invoker.GetFailedCalls().size(), 0u);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    ASSERT_EQ(replayed.size() + completed.size(), 5u);

    std::vector<MemRpc::RpcReply> replayCompleted;
    invoker.CollectResults(&replayCompleted);
    ASSERT_EQ(replayCompleted.size(), replayed.size());
    EXPECT_EQ(invoker.GetFailedCalls().size(), 1u);
    if (!invoker.GetFailedCalls().empty()) {
        EXPECT_EQ(invoker.GetFailedCalls()[0].opcode, static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest));
    }

    std::vector<int32_t> sums;
    sums.reserve(completed.size() + replayCompleted.size());
    auto collectSums = [&sums](const std::vector<MemRpc::RpcReply>& replies) {
        for (const auto& reply : replies) {
            EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
            AddReply decoded;
            ASSERT_TRUE(MemRpc::DecodeMessage(reply.payload, &decoded));
            sums.push_back(decoded.sum);
        }
    };
    collectSums(completed);
    collectSums(replayCompleted);

    ASSERT_EQ(sums.size(), 5u);
    std::sort(sums.begin(), sums.end());
    for (int i = 0; i < 5; ++i) {
        const auto reply_sum = sums[static_cast<size_t>(i)];
        const int expected_sum = 100 + i;
        EXPECT_EQ(reply_sum, expected_sum);
    }

    for (const auto& reply : replayCompleted) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, ReplayPolicyResubmitsInFlightCallsAfterCrash)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto replayNonFault = [](const FailedCallRecord& record) {
        EXPECT_EQ(record.recoveryEvent.trigger, MemRpc::RecoveryTrigger::EngineDeath);
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest) ? ReplayDecision::Skip
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

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
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
        EXPECT_EQ(invoker.GetFailedCalls()[0].opcode, static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest));
    }

    for (const auto& reply : replayCompleted) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, ReplayPolicySkipsSelectedCalls)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    auto skipAdd = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<MemRpc::Opcode>(TestkitOpcode::Add) ? ReplayDecision::Skip
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

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
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

TEST(TestkitDfxTest, HangingChildKilledAndRecovered)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto hangFuture = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::HangForTest)));

    usleep(100000);
    KillAndReap(child);
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply hangReply;
    EXPECT_EQ(std::move(hangFuture).Wait(&hangReply), kExpectedEngineDeathStatus);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(10, 20);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).Wait(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 30);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, OomKilledChildRecovery)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto oomFuture = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::OomForTest)));

    int status = 0;
    int waitResult = 0;
    for (int i = 0; i < 30; ++i) {
        waitResult = waitpid(child, &status, WNOHANG);
        if (waitResult > 0)
            break;
        usleep(100000);
    }
    if (waitResult == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply oomReply;
    EXPECT_EQ(std::move(oomFuture).Wait(&oomReply), kExpectedEngineDeathStatus);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(7, 8);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).Wait(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 15);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, StackOverflowChildRecovery)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto future = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::StackOverflowForTest)));

    int status = 0;
    int waitResult = 0;
    for (int i = 0; i < 30; ++i) {
        waitResult = waitpid(child, &status, WNOHANG);
        if (waitResult > 0)
            break;
        usleep(100000);
    }
    if (waitResult == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply reply;
    EXPECT_EQ(std::move(future).Wait(&reply), kExpectedEngineDeathStatus);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok);
    CloseHandles(handles);
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    const auto addCall = MakeAddCall(3, 4);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).Wait(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 7);

    client.Shutdown();
    KillAndReap(child2);
}

TEST(TestkitDfxTest, BatchPartialCompletionTracking)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

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
        EXPECT_EQ(failedCall.failureStatus, kExpectedEngineDeathStatus);
        EXPECT_EQ(failedCall.runtimeSnapshot.lastTrigger, MemRpc::RecoveryTrigger::EngineDeath);
    }

    invoker.Shutdown();
}

TEST(TestkitDfxTest, MultipleConsecutiveCrashesAndRecoveries)
{
    if (ThreadSanitizerEnabled()) {
        GTEST_SKIP() << "fork-based DFX tests are unsupported under ThreadSanitizer";
    }

    auto bootstrap = CreateBootstrap();

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    for (int cycle = 0; cycle < 3; ++cycle) {
        pid_t child = ForkServer(bootstrap);
        ASSERT_GT(child, 0) << "cycle " << cycle;

        const auto addCall = MakeAddCall(cycle, 10);
        MemRpc::RpcReply addReply;
        ASSERT_EQ(client.InvokeAsync(addCall).Wait(&addReply), MemRpc::StatusCode::Ok) << "cycle " << cycle;
        AddReply decoded;
        ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
        EXPECT_EQ(decoded.sum, cycle + 10);

        auto crashFuture = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(TestkitOpcode::CrashForTest)));
        waitpid(child, nullptr, 0);
        bootstrap->SimulateEngineDeathForTest();

        MemRpc::RpcReply crashReply;
        EXPECT_EQ(std::move(crashFuture).Wait(&crashReply), kExpectedEngineDeathStatus) << "cycle " << cycle;

        MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
        ASSERT_EQ(bootstrap->OpenSession(handles), MemRpc::StatusCode::Ok) << "cycle " << cycle;
        CloseHandles(handles);
    }

    pid_t finalChild = ForkServer(bootstrap);
    ASSERT_GT(finalChild, 0);

    const auto addCall = MakeAddCall(99, 1);
    MemRpc::RpcReply addReply;
    ASSERT_EQ(client.InvokeAsync(addCall).Wait(&addReply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(MemRpc::DecodeMessage(addReply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 100);

    client.Shutdown();
    KillAndReap(finalChild);
}

TEST(TestkitDfxTest, FailureMonitorTriggersAfterExecTimeoutThreshold)
{
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
