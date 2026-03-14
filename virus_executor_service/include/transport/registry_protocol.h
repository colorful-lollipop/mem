#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_PROTOCOL_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_PROTOCOL_H_

#include <cstdint>
#include <string>

namespace VirusExecutorService {

enum class RegistryOp : uint8_t {
    Register = 1,
    Get = 2,
    Load = 3,
    Unload = 4,
};

struct RegistryRequest {
    RegistryOp op{};
    int32_t sa_id = 0;
    std::string payload;
};

struct RegistryResponse {
    int32_t err_code = 0;
    std::string payload;
};

bool EncodeRegistryRequest(const RegistryRequest& req, std::string* out);
bool DecodeRegistryRequest(const uint8_t* data, size_t len, RegistryRequest* req);

bool EncodeRegistryResponse(const RegistryResponse& resp, std::string* out);
bool DecodeRegistryResponse(const uint8_t* data, size_t len, RegistryResponse* resp);

bool SendMessage(int fd, const std::string& msg);
bool RecvMessage(int fd, std::string* msg);

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_PROTOCOL_H_
