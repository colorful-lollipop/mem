#ifndef VPSDEMO_VES_BOOTSTRAP_STUB_H_
#define VPSDEMO_VES_BOOTSTRAP_STUB_H_

#include <cstring>

#include "iremote_stub.h"
#include "mock_ipc_types.h"
#include "memrpc/core/bootstrap.h"
#include "vpsdemo/transport/ves_bootstrap_interface.h"

namespace vpsdemo {

namespace {

struct SessionMetadata {
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

}  // namespace

// Command dispatch layer — mirrors real OHOS XXXStub::OnRemoteRequest.
// Maps IPC commands to IVesBootstrap interface methods.
class VesBootstrapStub : public OHOS::IRemoteStub<IVesBootstrap> {
 public:
    bool OnRemoteRequest(int command, OHOS::MockIpcReply* reply) override {
        switch (command) {
            case 1: {  // OpenSession
                memrpc::BootstrapHandles handles{};
                if (OpenSession(handles) != memrpc::StatusCode::Ok) {
                    return false;
                }

                constexpr size_t FD_COUNT = 6;
                reply->fds[0] = handles.shmFd;
                reply->fds[1] = handles.highReqEventFd;
                reply->fds[2] = handles.normalReqEventFd;
                reply->fds[3] = handles.respEventFd;
                reply->fds[4] = handles.reqCreditEventFd;
                reply->fds[5] = handles.respCreditEventFd;
                reply->fd_count = FD_COUNT;

                SessionMetadata meta{};
                meta.protocol_version = handles.protocolVersion;
                meta.session_id = handles.sessionId;
                std::memcpy(reply->data, &meta, sizeof(meta));
                reply->data_len = sizeof(meta);
                return true;
            }
            case 2:  // CloseSession
                return CloseSession() == memrpc::StatusCode::Ok;
            case 3: {  // Heartbeat
                VesHeartbeatReply hb{};
                if (Heartbeat(hb) != memrpc::StatusCode::Ok) {
                    return false;
                }
                std::memcpy(reply->data, &hb, sizeof(hb));
                reply->data_len = sizeof(hb);
                reply->close_after_reply = true;
                return true;
            }
            default:
                return false;
        }
    }
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_BOOTSTRAP_STUB_H_
