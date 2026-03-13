#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace virus_executor_service {

TEST(VesPolicyTest, ExecTimeoutTriggersOnFailure) {
    auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
    memrpc::BootstrapHandles unused{};
    ASSERT_EQ(bootstrap->OpenSession(unused), memrpc::StatusCode::Ok);

    memrpc::RpcServer server(bootstrap->serverHandles());
    memrpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<memrpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest& req) {
            // Force exec timeout by sleeping longer than client timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = 0;
            (void)req;
            return reply;
        });
    ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

    std::atomic<bool> failureCalled{false};
    memrpc::RpcClient client(bootstrap);

    memrpc::RecoveryPolicy policy;
    policy.onFailure = [&](const memrpc::RpcFailure& failure) {
        if (failure.status == memrpc::StatusCode::ExecTimeout) {
            failureCalled.store(true);
        }
        return memrpc::RecoveryDecision{memrpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

    ScanFileRequest req;
    req.filePath = "/data/sleep50.bin";

    memrpc::RpcCall call;
    call.opcode = static_cast<memrpc::Opcode>(VesOpcode::ScanFile);
    call.execTimeoutMs = 5;  // short timeout
    memrpc::CodecTraits<ScanFileRequest>::Encode(req, &call.payload);

    auto future = client.InvokeAsync(call);
    memrpc::RpcReply reply;
    auto status = future.Wait(&reply);

    EXPECT_EQ(status, memrpc::StatusCode::ExecTimeout);
    EXPECT_TRUE(failureCalled.load());

    client.Shutdown();
    server.Stop();
}

}  // namespace virus_executor_service
