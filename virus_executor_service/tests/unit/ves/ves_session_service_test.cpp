#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/codec.h"
#include "memrpc/core/types.h"
#include "ves/ves_codec.h"
#include "ves/ves_session_service.h"
#include "ves/ves_engine_service.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

using VirusExecutorService::VesEngineService;
using VirusExecutorService::EngineSessionService;

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
}  // namespace

TEST(VesSessionServiceTest, OpenSessionCreatesSessionHandles) {
    VesEngineService service;
    EngineSessionService sessionService({&service});

    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);

    CloseHandles(&handles);
}

TEST(VesSessionServiceTest, OpenSessionLeavesRegistrarStateUntouched) {
    VesEngineService service;
    EXPECT_FALSE(service.initialized());

    EngineSessionService sessionService({&service});
    MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();

    EXPECT_EQ(sessionService.OpenSession(handles), MemRpc::StatusCode::Ok);
    EXPECT_FALSE(service.initialized());

    CloseHandles(&handles);
}

TEST(VesSessionServiceTest, InvokeAnyCallUsesRegisteredTypedHandlers) {
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
