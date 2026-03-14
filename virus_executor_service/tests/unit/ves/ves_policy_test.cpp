#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"
#include "memrpc/server/typed_handler.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

TEST(VesPolicyTest, ExecTimeoutTriggersOnFailure) {
    auto bootstrap = std::make_shared<MemRpc::DevBootstrapChannel>();
    MemRpc::BootstrapHandles unused{};
    ASSERT_EQ(bootstrap->OpenSession(unused), MemRpc::StatusCode::Ok);

    MemRpc::RpcServer server(bootstrap->serverHandles());
    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        &server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [](const ScanFileRequest& req) {
            // Force exec timeout by sleeping longer than client timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ScanFileReply reply;
            reply.code = 0;
            reply.threatLevel = 0;
            (void)req;
            return reply;
        });
    ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

    std::atomic<bool> failureCalled{false};
    MemRpc::RpcClient client(bootstrap);

    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [&](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            failureCalled.store(true);
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    client.SetRecoveryPolicy(std::move(policy));

    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanFileRequest req;
    req.filePath = "/data/sleep50.bin";

    MemRpc::RpcCall call;
    call.opcode = static_cast<MemRpc::Opcode>(VesOpcode::ScanFile);
    call.execTimeoutMs = 5;  // short timeout
    MemRpc::CodecTraits<ScanFileRequest>::Encode(req, &call.payload);

    auto future = client.InvokeAsync(call);
    MemRpc::RpcReply reply;
    auto status = future.Wait(&reply);

    EXPECT_EQ(status, MemRpc::StatusCode::ExecTimeout);
    EXPECT_TRUE(failureCalled.load());

    client.Shutdown();
    server.Stop();
}

}  // namespace VirusExecutorService
