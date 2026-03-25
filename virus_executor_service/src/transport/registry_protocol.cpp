#include "transport/registry_protocol.h"

#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

#include <cerrno>

#include "virus_protection_service_log.h"

namespace VirusExecutorService {

// Wire: [Op:1][SaId:4][PayloadLen:2][Payload:N]
bool EncodeRegistryRequest(const RegistryRequest& req, std::string* out)
{
    if (out == nullptr || req.payload.size() > 0xFFFF) {
        HILOGE("EncodeRegistryRequest failed: out=%{public}p payload_size=%{public}zu", out, req.payload.size());
        return false;
    }
    const auto plen = static_cast<uint16_t>(req.payload.size());
    out->resize(1 + 4 + 2 + plen);
    auto* p = reinterpret_cast<uint8_t*>(out->data());
    p[0] = static_cast<uint8_t>(req.op);
    std::memcpy(p + 1, &req.sa_id, 4);
    std::memcpy(p + 5, &plen, 2);
    if (plen > 0) {
        std::copy_n(req.payload.data(), plen, p + 7);
    }
    return true;
}

bool DecodeRegistryRequest(const uint8_t* data, size_t len, RegistryRequest* req)
{
    if (data == nullptr || req == nullptr || len < 7) {
        HILOGE("DecodeRegistryRequest failed: data=%{public}p req=%{public}p len=%{public}zu", data, req, len);
        return false;
    }
    req->op = static_cast<RegistryOp>(data[0]);
    std::memcpy(&req->sa_id, data + 1, 4);
    uint16_t plen = 0;
    std::memcpy(&plen, data + 5, 2);
    if (len < 7u + plen) {
        HILOGE("DecodeRegistryRequest failed: len=%{public}zu payload_len=%{public}u", len, plen);
        return false;
    }
    req->payload.assign(reinterpret_cast<const char*>(data + 7), plen);
    return true;
}

// Wire: [ErrCode:4][PayloadLen:2][Payload:N]
bool EncodeRegistryResponse(const RegistryResponse& resp, std::string* out)
{
    if (out == nullptr || resp.payload.size() > 0xFFFF) {
        HILOGE("EncodeRegistryResponse failed: out=%{public}p payload_size=%{public}zu", out, resp.payload.size());
        return false;
    }
    const auto plen = static_cast<uint16_t>(resp.payload.size());
    out->resize(4 + 2 + plen);
    auto* p = reinterpret_cast<uint8_t*>(out->data());
    std::memcpy(p, &resp.err_code, 4);
    std::memcpy(p + 4, &plen, 2);
    if (plen > 0) {
        std::copy_n(resp.payload.data(), plen, p + 6);
    }
    return true;
}

bool DecodeRegistryResponse(const uint8_t* data, size_t len, RegistryResponse* resp)
{
    if (data == nullptr || resp == nullptr || len < 6) {
        HILOGE("DecodeRegistryResponse failed: data=%{public}p resp=%{public}p len=%{public}zu", data, resp, len);
        return false;
    }
    std::memcpy(&resp->err_code, data, 4);
    uint16_t plen = 0;
    std::memcpy(&plen, data + 4, 2);
    if (len < 6u + plen) {
        HILOGE("DecodeRegistryResponse failed: len=%{public}zu payload_len=%{public}u", len, plen);
        return false;
    }
    resp->payload.assign(reinterpret_cast<const char*>(data + 6), plen);
    return true;
}

// Length-prefixed I/O: [Len:4][Data:N]
bool SendMessage(int fd, const std::string& msg)
{
    auto len = static_cast<uint32_t>(msg.size());
    if (send(fd, &len, 4, MSG_NOSIGNAL) != 4) {
        HILOGE("SendMessage failed sending length: fd=%{public}d len=%{public}u errno=%{public}d", fd, len, errno);
        return false;
    }
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            HILOGE(
                "SendMessage failed sending payload: fd=%{public}d sent=%{public}zu total=%{public}zu errno=%{public}d",
                fd,
                sent,
                msg.size(),
                errno);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool RecvMessage(int fd, std::string* msg)
{
    if (msg == nullptr) {
        HILOGE("RecvMessage failed: msg is null");
        return false;
    }
    uint32_t len = 0;
    ssize_t n = recv(fd, &len, 4, MSG_WAITALL);
    if (n != 4 || len > 64 * 1024) {
        HILOGE("RecvMessage failed reading length: fd=%{public}d received=%{public}zd len=%{public}u errno=%{public}d",
               fd,
               n,
               len,
               errno);
        return false;
    }
    msg->resize(len);
    if (len == 0) {
        return true;
    }
    size_t received = 0;
    while (received < len) {
        n = recv(fd, msg->data() + received, len - received, 0);
        if (n <= 0) {
            HILOGE(
                "RecvMessage failed reading payload: fd=%{public}d received=%{public}zu len=%{public}u "
                "errno=%{public}d",
                fd,
                received,
                len,
                errno);
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace VirusExecutorService
