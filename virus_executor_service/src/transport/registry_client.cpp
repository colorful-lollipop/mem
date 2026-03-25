#include "transport/registry_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

#include <cerrno>

#include "transport/registry_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

RegistryClient::RegistryClient(const std::string& registrySocketPath)
    : registry_socket_path_(registrySocketPath)
{
}

// Returns response payload on success (may be empty), or sentinel "\x01" on
// connection/protocol error. err_code!=0 returns "\x01" too.
std::string RegistryClient::SendRequest(RegistryOp op, int32_t sa_id, const std::string& payload)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        HILOGE("RegistryClient::SendRequest socket failed: op=%{public}d sa_id=%{public}d errno=%{public}d",
               static_cast<int>(op),
               sa_id,
               errno);
        return {"\x01"};
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, registry_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        HILOGE(
            "RegistryClient::SendRequest connect failed: op=%{public}d sa_id=%{public}d socket=%{public}s "
            "errno=%{public}d",
            static_cast<int>(op),
            sa_id,
            registry_socket_path_.c_str(),
            errno);
        close(fd);
        return {"\x01"};
    }

    RegistryRequest req;
    req.op = op;
    req.sa_id = sa_id;
    req.payload = payload;

    std::string msg;
    if (!EncodeRegistryRequest(req, &msg) || !SendMessage(fd, msg)) {
        HILOGE("RegistryClient::SendRequest send failed: op=%{public}d sa_id=%{public}d payload_size=%{public}zu",
               static_cast<int>(op),
               sa_id,
               payload.size());
        close(fd);
        return {"\x01"};
    }

    std::string resp_msg;
    if (!RecvMessage(fd, &resp_msg)) {
        HILOGE("RegistryClient::SendRequest recv failed: op=%{public}d sa_id=%{public}d", static_cast<int>(op), sa_id);
        close(fd);
        return {"\x01"};
    }
    close(fd);

    RegistryResponse resp;
    if (!DecodeRegistryResponse(reinterpret_cast<const uint8_t*>(resp_msg.data()), resp_msg.size(), &resp)) {
        HILOGE("RegistryClient::SendRequest decode failed: op=%{public}d sa_id=%{public}d resp_size=%{public}zu",
               static_cast<int>(op),
               sa_id,
               resp_msg.size());
        return {"\x01"};
    }

    if (resp.err_code != 0) {
        HILOGE("RegistryClient::SendRequest remote error: op=%{public}d sa_id=%{public}d err_code=%{public}d",
               static_cast<int>(op),
               sa_id,
               resp.err_code);
        return {"\x01"};
    }
    return resp.payload;
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
