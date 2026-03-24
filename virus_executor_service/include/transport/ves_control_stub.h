#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_STUB_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_STUB_H_

#include <cstring>

#include "iremote_stub.h"
#include "mock_ipc_types.h"
#include "memrpc/core/bootstrap.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"

namespace VirusExecutorService {

namespace detail {

struct SessionMetadata {
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

struct AnyCallRequestHeader {
    uint16_t opcode = 0;
    uint16_t priority = static_cast<uint16_t>(MemRpc::Priority::Normal);
    uint32_t timeoutMs = 0;
    uint32_t payloadSize = 0;
};

struct AnyCallReplyHeader {
    uint32_t status = 0;
    int32_t errorCode = 0;
    uint32_t payloadSize = 0;
};

inline bool DecodeAnyCallRequest(const OHOS::MockIpcRequest& request, VesAnyCallRequest* out)
{
    if (out == nullptr || request.data.size() < sizeof(AnyCallRequestHeader)) {
        return false;
    }
    AnyCallRequestHeader header{};
    std::memcpy(&header, request.data.data(), sizeof(header));
    if (request.data.size() != sizeof(header) + header.payloadSize) {
        return false;
    }
    out->opcode = header.opcode;
    out->priority = header.priority;
    out->timeoutMs = header.timeoutMs;
    out->payload.assign(request.data.begin() + static_cast<std::ptrdiff_t>(sizeof(header)),
                        request.data.end());
    return true;
}

inline bool DecodeOpenSessionRequest(const OHOS::MockIpcRequest& request,
                                     VesOpenSessionRequest* out)
{
    return out != nullptr &&
           MemRpc::DecodeMessage<VesOpenSessionRequest>(request.data, out) &&
           IsValidVesOpenSessionRequest(*out);
}

inline void EncodeAnyCallReply(const VesAnyCallReply& reply, std::vector<uint8_t>* out)
{
    if (out == nullptr) {
        return;
    }
    AnyCallReplyHeader header{};
    header.status = static_cast<uint32_t>(reply.status);
    header.errorCode = reply.errorCode;
    header.payloadSize = static_cast<uint32_t>(reply.payload.size());
    out->resize(sizeof(header) + reply.payload.size());
    std::memcpy(out->data(), &header, sizeof(header));
    if (!reply.payload.empty()) {
        std::memcpy(out->data() + sizeof(header), reply.payload.data(), reply.payload.size());
    }
}

}  // namespace detail

class VesControlStub : public OHOS::IRemoteStub<IVirusProtectionExecutor> {
 public:
    bool OnRemoteRequest(int command, const OHOS::MockIpcRequest& request,
                         OHOS::MockIpcReply* reply) override {
        switch (command) {
            case 1: {
                VesOpenSessionRequest openRequest{};
                if (!detail::DecodeOpenSessionRequest(request, &openRequest)) {
                    return false;
                }
                MemRpc::BootstrapHandles handles = MemRpc::MakeDefaultBootstrapHandles();
                if (OpenSession(openRequest, handles) != MemRpc::StatusCode::Ok) {
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

                detail::SessionMetadata meta{};
                meta.protocol_version = handles.protocolVersion;
                meta.session_id = handles.sessionId;
                reply->data.resize(sizeof(meta));
                std::memcpy(reply->data.data(), &meta, sizeof(meta));
                return true;
            }
            case 2:
                reply->close_after_reply = true;
                return CloseSession() == MemRpc::StatusCode::Ok;
            case 3: {
                VesHeartbeatReply hb{};
                if (Heartbeat(hb) != MemRpc::StatusCode::Ok) {
                    return false;
                }
                reply->data.resize(sizeof(hb));
                std::memcpy(reply->data.data(), &hb, sizeof(hb));
                reply->close_after_reply = true;
                return true;
            }
            case 4: {
                VesAnyCallRequest anyRequest{};
                if (!detail::DecodeAnyCallRequest(request, &anyRequest)) {
                    return false;
                }
                VesAnyCallReply anyReply{};
                if (AnyCall(anyRequest, anyReply) != MemRpc::StatusCode::Ok) {
                    return false;
                }
                detail::EncodeAnyCallReply(anyReply, &reply->data);
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
