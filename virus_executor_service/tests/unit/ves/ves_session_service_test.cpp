#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/codec.h"
#include "memrpc/core/types.h"
#include "ves/ves_codec.h"
#include "ves/ves_engine_service.h"
#include "ves/ves_protocol.h"
#include "ves/ves_session_service.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

using VirusExecutorService::EngineSessionService;
using VirusExecutorService::VesEngineService;

namespace {
class TrackingSessionHost final : public MemRpc::IServerSessionHost {
public:
    explicit TrackingSessionHost(MemRpc::SharedMemorySessionConfig config = {})
        : inner_(std::make_shared<MemRpc::SharedMemorySessionHost>(std::move(config)))
    {
    }

    MemRpc::StatusCode EnsureSession() override
    {
        ++ensureCount_;
        return inner_->EnsureSession();
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        ++openCount_;
        return inner_->OpenSession(handles);
    }

    MemRpc::StatusCode CloseSession() override
    {
        ++closeCount_;
        return inner_->CloseSession();
    }

    [[nodiscard]] MemRpc::BootstrapHandles serverHandles() const override
    {
        ++serverHandlesCount_;
        return inner_->serverHandles();
    }

    [[nodiscard]] int ensureCount() const
    {
        return ensureCount_.load();
    }

    [[nodiscard]] int openCount() const
    {
        return openCount_.load();
    }

    [[nodiscard]] int closeCount() const
    {
        return closeCount_.load();
    }

    [[nodiscard]] int serverHandlesCount() const
    {
        return serverHandlesCount_.load();
    }

private:
    std::shared_ptr<MemRpc::SharedMemorySessionHost> inner_;
    mutable std::atomic<int> ensureCount_{0};
    mutable std::atomic<int> openCount_{0};
    mutable std::atomic<int> closeCount_{0};
    mutable std::atomic<int> serverHandlesCount_{0};
};

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
}  // namespace

TEST(VesSessionServiceTest, OpenSessionCreatesSessionHandles)
{
    VesEngineService service;
    EngineSessionService sessionService({&service});

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);

    CloseHandles(&handles);
}

TEST(VesSessionServiceTest, OpenSessionLeavesRegistrarStateUntouched)
{
    VesEngineService service;
    EXPECT_FALSE(service.initialized());

    EngineSessionService sessionService({&service});
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_FALSE(service.initialized());

    CloseHandles(&handles);
}

TEST(VesSessionServiceTest, OpenSessionUsesInjectedServerSessionHost)
{
    VesEngineService service;
    auto sessionHost = std::make_shared<TrackingSessionHost>();
    EngineSessionService sessionService({&service}, sessionHost);

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_GE(sessionHost->ensureCount(), 1);
    EXPECT_GE(sessionHost->serverHandlesCount(), 1);
    EXPECT_GE(sessionHost->openCount(), 1);

    CloseHandles(&handles);
    EXPECT_EQ(sessionService.CloseSession(), MemRpc::StatusCode::Ok);
    EXPECT_GE(sessionHost->closeCount(), 1);
}

TEST(VesSessionServiceTest, InvokeAnyCallUsesRegisteredTypedHandlers)
{
    VesEngineService service;
    service.Initialize();

    EngineSessionService sessionService({&service});

    ScanTask request{"/data/eicar_via_anycall.apk"};
    std::vector<uint8_t> payload;
    ASSERT_TRUE(MemRpc::EncodeMessage(request, &payload));

    MemRpc::RpcServerCall call;
    call.opcode = static_cast<MemRpc::Opcode>(VesOpcode::ScanFile);
    call.payload = MemRpc::PayloadView(payload.data(), payload.size());

    MemRpc::RpcServerReply reply;
    ASSERT_EQ(sessionService.InvokeAnyCall(call, &reply), MemRpc::StatusCode::Ok);

    ScanFileReply scanReply;
    ASSERT_TRUE(MemRpc::DecodeMessage(reply.payload, &scanReply));
    EXPECT_EQ(scanReply.code, 0);
    EXPECT_EQ(scanReply.threatLevel, 1);
}

}  // namespace VirusExecutorService
