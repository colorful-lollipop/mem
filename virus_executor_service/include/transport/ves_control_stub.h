#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_STUB_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_STUB_H_

#include <cstring>

#include "iremote_stub.h"
#include "mock_ipc_types.h"
#include "memrpc/core/bootstrap.h"
#include "transport/ves_control_interface.h"

namespace VirusExecutorService {

namespace {

struct SessionMetadata {
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

}  // namespace

class VesControlStub : public OHOS::IRemoteStub<IVesControl> {
 public:
    bool OnRemoteRequest(int command, OHOS::MockIpcReply* reply) override {
        switch (command) {
            case 1: {
                MemRpc::BootstrapHandles handles{};
                if (OpenSession(handles) != MemRpc::StatusCode::Ok) {
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
            case 2:
                return CloseSession() == MemRpc::StatusCode::Ok;
            case 3: {
                VesHeartbeatReply hb{};
                if (Heartbeat(hb) != MemRpc::StatusCode::Ok) {
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

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_STUB_H_
