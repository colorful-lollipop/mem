#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/common/minirpc_codec.h"
#include "apps/minirpc/parent/minirpc_failure_monitor.h"
#include "minirpc_resilient_invoker.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

#include "apps/minirpc/protocol.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& h) {
    if (h.shmFd >= 0) close(h.shmFd);
    if (h.highReqEventFd >= 0) close(h.highReqEventFd);
    if (h.normalReqEventFd >= 0) close(h.normalReqEventFd);
    if (h.respEventFd >= 0) close(h.respEventFd);
    if (h.reqCreditEventFd >= 0) close(h.reqCreditEventFd);
    if (h.respCreditEventFd >= 0) close(h.respCreditEventFd);
}

void RunMiniRpcServerProcess(MemRpc::BootstrapHandles handles) {
    MemRpc::RpcServer server;
    server.SetBootstrapHandles(handles);
    MiniRpcService service;
    service.RegisterHandlers(&server);
    if (server.Start() != MemRpc::StatusCode::Ok) {
        _exit(2);
    }
    server.Run();
    _exit(0);
}

pid_t ForkServer(std::shared_ptr<MemRpc::PosixDemoBootstrapChannel> bootstrap) {
    pid_t child = fork();
    if (child == 0) {
        RunMiniRpcServerProcess(bootstrap->serverHandles());
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
    EncodeMessage<AddRequest>(request, &payload);
    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniAdd);
    call.payload = std::move(payload);
    return call;
}

MemRpc::RpcCall MakeSleepCall(uint32_t delay_ms) {
    SleepRequest request;
    request.delay_ms = delay_ms;
    std::vector<uint8_t> payload;
    EncodeMessage<SleepRequest>(request, &payload);
    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniSleep);
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

// ---------------------------------------------------------------------------
// Test: CrashDuringBatchTracksAllFailures
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, CrashDuringBatchTracksAllFailures) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    MemRpc::BootstrapHandles unused_handles;
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
    CloseHandles(unused_handles);
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 10; ++i) {
        batch.push_back(MakeAddCall(i, i + 1));
    }
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest)));

    invoker.SubmitBatch(batch);

    int status = 0;
    waitpid(child, &status, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    EXPECT_GT(failed.size(), 0u);
    EXPECT_EQ(completed.size() + failed.size(), batch.size());

    for (const auto& f : failed) {
        EXPECT_EQ(f.failure_status, MemRpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

// ---------------------------------------------------------------------------
// Test: ReplayPolicyResubmitsAfterCrash
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, ReplayPolicyResubmitsAfterCrash) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    // Replay everything except crash calls (we don't want to crash the new child).
    auto replay_non_fault = [](const FailedCallRecord& r) {
        return r.opcode == static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest) ? ReplayDecision::Skip
                                                            : ReplayDecision::Replay;
    };
    ResilientBatchInvoker invoker(bootstrap, replay_non_fault);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    // Submit Add calls + crash.
    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 5; ++i) {
        batch.push_back(MakeAddCall(i, 100));
    }
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    ASSERT_GT(invoker.GetFailedCalls().size(), 0u);

    // Restart child.
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    EXPECT_GT(replayed.size(), 0u);

    std::vector<MemRpc::RpcReply> replay_completed;
    invoker.CollectResults(&replay_completed);
    // Only the skipped crash call should remain in failed list.
    EXPECT_EQ(invoker.GetFailedCalls().size(), 1u);
    if (!invoker.GetFailedCalls().empty()) {
        EXPECT_EQ(invoker.GetFailedCalls()[0].opcode, static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest));
    }

    for (const auto& reply : replay_completed) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }

    invoker.Shutdown();
    KillAndReap(child2);
}

// ---------------------------------------------------------------------------
// Test: ReplayPolicySkipsSelectedCalls
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, ReplayPolicySkipsSelectedCalls) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    // Skip Add calls, replay everything else.
    auto skip_add = [](const FailedCallRecord& record) {
        return record.opcode == static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniAdd) ? ReplayDecision::Skip
                                                        : ReplayDecision::Replay;
    };

    ResilientBatchInvoker invoker(bootstrap, skip_add);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    std::vector<MemRpc::RpcCall> batch;
    batch.push_back(MakeAddCall(1, 2));
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);
    size_t total_failed = invoker.GetFailedCalls().size();
    ASSERT_GT(total_failed, 0u);

    // Count how many failed Add calls there are.
    size_t add_failures = 0;
    for (const auto& f : invoker.GetFailedCalls()) {
        if (f.opcode == static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniAdd)) {
            ++add_failures;
        }
    }

    // Restart and replay.
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto replayed = invoker.ReplayFailed();
    // Replayed should not include skipped Add calls.
    EXPECT_EQ(replayed.size(), total_failed - add_failures);
    // Skipped Add calls remain in failed list.
    EXPECT_EQ(invoker.GetFailedCalls().size(), add_failures);

    invoker.Shutdown();
    KillAndReap(child2);
}

// ---------------------------------------------------------------------------
// Test: HangingChildKilledAndRecovered
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, HangingChildKilledAndRecovered) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto hang_future = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniHangForTest)));

    // Give child time to enter handler, then kill it.
    usleep(100000);
    KillAndReap(child);
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply hang_reply;
    EXPECT_EQ(hang_future.Wait(&hang_reply), MemRpc::StatusCode::PeerDisconnected);

    // Restart and verify recovery.
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto add_call = MakeAddCall(10, 20);
    MemRpc::RpcReply add_reply;
    ASSERT_EQ(client.InvokeAsync(add_call).WaitAndTake(&add_reply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(DecodeMessage<AddReply>(add_reply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 30);

    client.Shutdown();
    KillAndReap(child2);
}

// ---------------------------------------------------------------------------
// Test: OomKilledChildRecovery
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, OomKilledChildRecovery) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto oom_future = client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniOomForTest)));

    // Wait for child to die from OOM (or time out and SIGKILL it).
    int status = 0;
    int wait_result = 0;
    for (int i = 0; i < 30; ++i) {
        wait_result = waitpid(child, &status, WNOHANG);
        if (wait_result > 0) break;
        usleep(100000);
    }
    if (wait_result == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply oom_reply;
    EXPECT_EQ(oom_future.Wait(&oom_reply), MemRpc::StatusCode::PeerDisconnected);

    // Restart and verify recovery.
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto add_call = MakeAddCall(7, 8);
    MemRpc::RpcReply add_reply;
    ASSERT_EQ(client.InvokeAsync(add_call).WaitAndTake(&add_reply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(DecodeMessage<AddReply>(add_reply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 15);

    client.Shutdown();
    KillAndReap(child2);
}

// ---------------------------------------------------------------------------
// Test: StackOverflowChildRecovery
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, StackOverflowChildRecovery) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    auto so_future =
        client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniStackOverflowForTest)));

    // Wait for child to die from SIGSEGV.
    int status = 0;
    int wait_result = 0;
    for (int i = 0; i < 30; ++i) {
        wait_result = waitpid(child, &status, WNOHANG);
        if (wait_result > 0) break;
        usleep(100000);
    }
    if (wait_result == 0) {
        KillAndReap(child);
    }
    bootstrap->SimulateEngineDeathForTest();

    MemRpc::RpcReply so_reply;
    EXPECT_EQ(so_future.Wait(&so_reply), MemRpc::StatusCode::PeerDisconnected);

    // Restart and verify recovery.
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child2 = ForkServer(bootstrap);
    ASSERT_GT(child2, 0);

    auto add_call = MakeAddCall(3, 4);
    MemRpc::RpcReply add_reply;
    ASSERT_EQ(client.InvokeAsync(add_call).WaitAndTake(&add_reply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(DecodeMessage<AddReply>(add_reply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 7);

    client.Shutdown();
    KillAndReap(child2);
}

// ---------------------------------------------------------------------------
// Test: BatchPartialCompletionTracking
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, BatchPartialCompletionTracking) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }
    pid_t child = ForkServer(bootstrap);
    ASSERT_GT(child, 0);

    ResilientBatchInvoker invoker(bootstrap);
    ASSERT_EQ(invoker.Init(), MemRpc::StatusCode::Ok);

    // Submit many fast Add calls + a slow Sleep + crash.
    std::vector<MemRpc::RpcCall> batch;
    for (int i = 0; i < 20; ++i) {
        batch.push_back(MakeAddCall(i, 1));
    }
    batch.push_back(MakeSleepCall(500));
    batch.push_back(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest)));
    invoker.SubmitBatch(batch);

    waitpid(child, nullptr, 0);
    bootstrap->SimulateEngineDeathForTest();

    std::vector<MemRpc::RpcReply> completed;
    invoker.CollectResults(&completed);

    const auto& failed = invoker.GetFailedCalls();
    // Some should have completed, some should have failed.
    EXPECT_EQ(completed.size() + failed.size(), batch.size());
    // At least the crash call itself should be in the failed set.
    EXPECT_GT(failed.size(), 0u);

    // Verify all completed replies are Ok.
    for (const auto& reply : completed) {
        EXPECT_EQ(reply.status, MemRpc::StatusCode::Ok);
    }
    // Verify all failed have PeerDisconnected.
    for (const auto& f : failed) {
        EXPECT_EQ(f.failure_status, MemRpc::StatusCode::PeerDisconnected);
    }

    invoker.Shutdown();
}

// ---------------------------------------------------------------------------
// Test: MultipleConsecutiveCrashesAndRecoveries
// ---------------------------------------------------------------------------
TEST(MiniRpcDfxTest, MultipleConsecutiveCrashesAndRecoveries) {
    auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
    {
        MemRpc::BootstrapHandles unused_handles;
        ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
        CloseHandles(unused_handles);
    }

    MemRpc::RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    for (int cycle = 0; cycle < 3; ++cycle) {
        pid_t child = ForkServer(bootstrap);
        ASSERT_GT(child, 0) << "cycle " << cycle;

        // Verify Add works.
        auto add_call = MakeAddCall(cycle, 10);
        MemRpc::RpcReply add_reply;
        ASSERT_EQ(client.InvokeAsync(add_call).WaitAndTake(&add_reply), MemRpc::StatusCode::Ok)
            << "cycle " << cycle;
        AddReply decoded;
        ASSERT_TRUE(DecodeMessage<AddReply>(add_reply.payload, &decoded));
        EXPECT_EQ(decoded.sum, cycle + 10);

        // Crash child.
        auto crash_future =
            client.InvokeAsync(MakeFaultCall(static_cast<MemRpc::Opcode>(MiniRpcOpcode::MiniCrashForTest)));
        waitpid(child, nullptr, 0);
        bootstrap->SimulateEngineDeathForTest();

        MemRpc::RpcReply crash_reply;
        EXPECT_EQ(crash_future.Wait(&crash_reply), MemRpc::StatusCode::PeerDisconnected)
            << "cycle " << cycle;

        // Restart for next cycle.
        {
            MemRpc::BootstrapHandles unused_handles;
            ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok) << "cycle " << cycle;
            CloseHandles(unused_handles);
        }
    }

    // Final verification with a fresh child.
    pid_t final_child = ForkServer(bootstrap);
    ASSERT_GT(final_child, 0);

    auto add_call = MakeAddCall(99, 1);
    MemRpc::RpcReply add_reply;
    ASSERT_EQ(client.InvokeAsync(add_call).WaitAndTake(&add_reply), MemRpc::StatusCode::Ok);
    AddReply decoded;
    ASSERT_TRUE(DecodeMessage<AddReply>(add_reply.payload, &decoded));
    EXPECT_EQ(decoded.sum, 100);

    client.Shutdown();
    KillAndReap(final_child);
}

TEST(MiniRpcDfxTest, FailureMonitorTriggersAfterExecTimeoutThreshold) {
    int triggered = 0;
    FailureMonitor::Options options;
    options.window_ms = 60000;
    options.exec_timeout_threshold = 3;

    FailureMonitor monitor(options, [&] { ++triggered; });

    MemRpc::RpcFailure failure;
    failure.status = MemRpc::StatusCode::ExecTimeout;

    monitor.OnFailure(failure);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 0);
    monitor.OnFailure(failure);
    EXPECT_EQ(triggered, 1);
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
