#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <memory>
#include <string>

#include "client/ves_client.h"
#include "memrpc/test_support/dev_bootstrap.h"
#include "memrpc/core/codec.h"
#include "memrpc/server/rpc_server.h"
#include "transport/ves_control_stub.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"

namespace VirusExecutorService {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles)
{
    if (handles.shmFd >= 0) {
        close(handles.shmFd);
    }
    if (handles.highReqEventFd >= 0) {
        close(handles.highReqEventFd);
    }
    if (handles.normalReqEventFd >= 0) {
        close(handles.normalReqEventFd);
    }
    if (handles.respEventFd >= 0) {
        close(handles.respEventFd);
    }
    if (handles.reqCreditEventFd >= 0) {
        close(handles.reqCreditEventFd);
    }
    if (handles.respCreditEventFd >= 0) {
        close(handles.respCreditEventFd);
    }
}

void RegisterScanHandler(MemRpc::RpcServer* server, std::atomic<int>* memrpcCount)
{
    if (server == nullptr) {
        return;
    }

    server->RegisterHandler(static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
                            [memrpcCount](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                                if (reply == nullptr) {
                                    return;
                                }

                                ScanTask request;
                                if (!MemRpc::DecodeMessage<ScanTask>(call.payload, &request)) {
                                    reply->status = MemRpc::StatusCode::ProtocolMismatch;
                                    return;
                                }

                                ScanFileReply scanReply;
                                scanReply.code = 0;
                                scanReply.threatLevel = request.path.find("fallback") != std::string::npos ? 2 : 1;
                                if (!MemRpc::EncodeMessage(scanReply, &reply->payload)) {
                                    reply->status = MemRpc::StatusCode::EngineInternalError;
                                    reply->payload.clear();
                                    return;
                                }

                                if (memrpcCount != nullptr) {
                                    memrpcCount->fetch_add(1);
                                }
                            });
}

class FakeClientControl final : public VesControlStub {
public:
    FakeClientControl()
        : bootstrap_(std::make_shared<MemRpc::DevBootstrapChannel>())
    {
        MemRpc::BootstrapHandles warmup = MemRpc::MakeDefaultBootstrapHandles();
        EXPECT_EQ(bootstrap_->OpenSession(warmup), MemRpc::StatusCode::Ok);
        CloseHandles(warmup);

        server_ = std::make_unique<MemRpc::RpcServer>(bootstrap_->serverHandles());
        RegisterScanHandler(server_.get(), &memrpcCount_);
        EXPECT_EQ(server_->Start(), MemRpc::StatusCode::Ok);
    }

    ~FakeClientControl() override
    {
        if (server_ != nullptr) {
            server_->Stop();
        }
    }

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest&, MemRpc::BootstrapHandles& handles) override
    {
        sessionOpen_.store(true);
        return bootstrap_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        sessionOpen_.store(false);
        return bootstrap_->CloseSession();
    }

    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override
    {
        reply = {};
        reply.version = 2;
        reply.sessionId = sessionOpen_.load() ? bootstrap_->serverHandles().sessionId : 0;
        reply.status = static_cast<uint32_t>(sessionOpen_.load() ? VesHeartbeatStatus::OkIdle
                                                                 : VesHeartbeatStatus::UnhealthyNoSession);
        reply.reasonCode = static_cast<uint32_t>(sessionOpen_.load() ? VesHeartbeatReasonCode::None
                                                                     : VesHeartbeatReasonCode::NoSession);
        reply.flags = sessionOpen_.load() ? (VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED) : 0;
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode AnyCall(const VesAnyCallRequest& request, VesAnyCallReply& reply) override
    {
        anyCallCount_.fetch_add(1);
        reply = {};
        if (static_cast<VesOpcode>(request.opcode) != VesOpcode::ScanFile) {
            reply.status = MemRpc::StatusCode::InvalidArgument;
            return MemRpc::StatusCode::Ok;
        }

        ScanTask scanTask;
        if (!MemRpc::DecodeMessage<ScanTask>(request.payload, &scanTask)) {
            reply.status = MemRpc::StatusCode::ProtocolMismatch;
            return MemRpc::StatusCode::Ok;
        }

        ScanFileReply scanReply;
        scanReply.code = 0;
        scanReply.threatLevel = 3;
        reply.status = MemRpc::StatusCode::Ok;
        if (!MemRpc::EncodeMessage(scanReply, &reply.payload)) {
            reply.status = MemRpc::StatusCode::EngineInternalError;
        }
        return MemRpc::StatusCode::Ok;
    }

    [[nodiscard]] int memrpcCount() const
    {
        return memrpcCount_.load();
    }

    [[nodiscard]] int anyCallCount() const
    {
        return anyCallCount_.load();
    }

private:
    std::shared_ptr<MemRpc::DevBootstrapChannel> bootstrap_;
    std::unique_ptr<MemRpc::RpcServer> server_;
    std::atomic<bool> sessionOpen_{false};
    std::atomic<int> memrpcCount_{0};
    std::atomic<int> anyCallCount_{0};
};

}  // namespace

TEST(VesTransportSelectionTest, SmallScanFileUsesMemrpcPath)
{
    auto control = std::make_shared<FakeClientControl>();
    VesClient client([control]() -> OHOS::sptr<IVirusProtectionExecutor> { return control; });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanTask task{"/data/small.bin"};
    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile(task, &reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 1);
    EXPECT_EQ(control->memrpcCount(), 1);
    EXPECT_EQ(control->anyCallCount(), 0);

    client.Shutdown();
}

TEST(VesTransportSelectionTest, OversizedScanFileUsesFallbackPath)
{
    auto control = std::make_shared<FakeClientControl>();
    VesClient client([control]() -> OHOS::sptr<IVirusProtectionExecutor> { return control; });
    ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

    ScanTask task{"/data/" + std::string(MemRpc::DEFAULT_MAX_REQUEST_BYTES + 128, 'x')};
    ScanFileReply reply;
    ASSERT_EQ(client.ScanFile(task, &reply), MemRpc::StatusCode::Ok);
    EXPECT_EQ(reply.threatLevel, 3);
    EXPECT_EQ(control->memrpcCount(), 0);
    EXPECT_EQ(control->anyCallCount(), 1);

    client.Shutdown();
}

}  // namespace VirusExecutorService
