#include "transport/registry_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

#include <cerrno>

#include "transport/registry_protocol.h"
#include "virus_protection_executor_log.h"

namespace VirusExecutorService {
namespace {

constexpr char REGISTRY_REQUEST_FAILED[] = "\x01";

bool ConnectToRegistry(const std::string& socketPath, RegistryOp op, int32_t saId, int* fd)
{
    *fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (*fd < 0) {
        HILOGE("RegistryClient::SendRequest socket failed: op=%{public}d sa_id=%{public}d errno=%{public}d",
               static_cast<int>(op),
               saId,
               errno);
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(*fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        HILOGE(
            "RegistryClient::SendRequest connect failed: op=%{public}d sa_id=%{public}d socket=%{public}s "
            "errno=%{public}d",
            static_cast<int>(op),
            saId,
            socketPath.c_str(),
            errno);
        close(*fd);
        *fd = -1;
        return false;
    }
    return true;
}

bool SendRegistryRequestMessage(int fd, RegistryOp op, int32_t saId, const std::string& payload)
{
    RegistryRequest req;
    req.op = op;
    req.sa_id = saId;
    req.payload = payload;

    std::string msg;
    if (EncodeRegistryRequest(req, &msg) && SendMessage(fd, msg)) {
        return true;
    }

    HILOGE("RegistryClient::SendRequest send failed: op=%{public}d sa_id=%{public}d payload_size=%{public}zu",
           static_cast<int>(op),
           saId,
           payload.size());
    return false;
}

bool RecvRegistryResponseMessage(int fd, RegistryOp op, int32_t saId, std::string* respMsg)
{
    if (RecvMessage(fd, respMsg)) {
        return true;
    }

    HILOGE("RegistryClient::SendRequest recv failed: op=%{public}d sa_id=%{public}d", static_cast<int>(op), saId);
    return false;
}

bool DecodeRegistryResponsePayload(const std::string& respMsg, RegistryOp op, int32_t saId, std::string* payload)
{
    RegistryResponse resp;
    if (!DecodeRegistryResponse(reinterpret_cast<const uint8_t*>(respMsg.data()), respMsg.size(), &resp)) {
        HILOGE("RegistryClient::SendRequest decode failed: op=%{public}d sa_id=%{public}d resp_size=%{public}zu",
               static_cast<int>(op),
               saId,
               respMsg.size());
        return false;
    }

    if (resp.err_code != 0) {
        HILOGE("RegistryClient::SendRequest remote error: op=%{public}d sa_id=%{public}d err_code=%{public}d",
               static_cast<int>(op),
               saId,
               resp.err_code);
        return false;
    }

    *payload = std::move(resp.payload);
    return true;
}

}  // namespace

RegistryClient::RegistryClient(const std::string& registrySocketPath)
    : registry_socket_path_(registrySocketPath)
{
}

// Returns response payload on success (may be empty), or sentinel "\x01" on
// connection/protocol error. err_code!=0 returns "\x01" too.
std::string RegistryClient::SendRequest(RegistryOp op, int32_t sa_id, const std::string& payload)
{
    int fd = -1;
    if (!ConnectToRegistry(registry_socket_path_, op, sa_id, &fd)) {
        return {REGISTRY_REQUEST_FAILED};
    }

    if (!SendRegistryRequestMessage(fd, op, sa_id, payload)) {
        close(fd);
        return {REGISTRY_REQUEST_FAILED};
    }

    std::string resp_msg;
    if (!RecvRegistryResponseMessage(fd, op, sa_id, &resp_msg)) {
        close(fd);
        return {REGISTRY_REQUEST_FAILED};
    }
    close(fd);

    std::string responsePayload;
    if (!DecodeRegistryResponsePayload(resp_msg, op, sa_id, &responsePayload)) {
        return {REGISTRY_REQUEST_FAILED};
    }
    return responsePayload;
}

std::string RegistryClient::GetServicePath(int32_t sa_id)
{
    std::string r = SendRequest(RegistryOp::Get, sa_id);
    return (r == "\x01") ? std::string{} : r;
}

std::string RegistryClient::LoadServicePath(int32_t sa_id)
{
    std::string r = SendRequest(RegistryOp::Load, sa_id);
    return (r == "\x01") ? std::string{} : r;
}

bool RegistryClient::UnloadService(int32_t sa_id)
{
    std::string r = SendRequest(RegistryOp::Unload, sa_id);
    return r != "\x01";
}

bool RegistryClient::RegisterService(int32_t sa_id, const std::string& serviceSocketPath)
{
    std::string r = SendRequest(RegistryOp::Register, sa_id, serviceSocketPath);
    return r != "\x01";
}

}  // namespace VirusExecutorService
