#include "virus_executor_service/transport/registry_client.h"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "virus_executor_service/transport/registry_protocol.h"

namespace virus_executor_service {

RegistryClient::RegistryClient(const std::string& registrySocketPath)
    : registry_socket_path_(registrySocketPath) {}

// Returns response payload on success (may be empty), or sentinel "\x01" on
// connection/protocol error. err_code!=0 returns "\x01" too.
std::string RegistryClient::SendRequest(uint8_t op, int32_t sa_id,
                                         const std::string& payload) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {"\x01"};
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, registry_socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return {"\x01"};
    }

    RegistryRequest req;
    req.op = static_cast<RegistryOp>(op);
    req.sa_id = sa_id;
    req.payload = payload;

    std::string msg;
    if (!EncodeRegistryRequest(req, &msg) || !SendMessage(fd, msg)) {
        close(fd);
        return {"\x01"};
    }

    std::string resp_msg;
    if (!RecvMessage(fd, &resp_msg)) {
        close(fd);
        return {"\x01"};
    }
    close(fd);

    RegistryResponse resp;
    if (!DecodeRegistryResponse(reinterpret_cast<const uint8_t*>(resp_msg.data()),
                                 resp_msg.size(), &resp)) {
        return {"\x01"};
    }

    if (resp.err_code != 0) {
        return {"\x01"};
    }
    return resp.payload;
}

std::string RegistryClient::GetServicePath(int32_t sa_id) {
    std::string r = SendRequest(static_cast<uint8_t>(RegistryOp::Get), sa_id);
    return (r == "\x01") ? std::string{} : r;
}

std::string RegistryClient::LoadServicePath(int32_t sa_id) {
    std::string r = SendRequest(static_cast<uint8_t>(RegistryOp::Load), sa_id);
    return (r == "\x01") ? std::string{} : r;
}

bool RegistryClient::UnloadService(int32_t sa_id) {
    std::string r = SendRequest(static_cast<uint8_t>(RegistryOp::Unload), sa_id);
    return r != "\x01";
}

bool RegistryClient::RegisterService(int32_t sa_id,
                                      const std::string& serviceSocketPath) {
    std::string r = SendRequest(static_cast<uint8_t>(RegistryOp::Register),
                                 sa_id, serviceSocketPath);
    return r != "\x01";
}

}  // namespace virus_executor_service
