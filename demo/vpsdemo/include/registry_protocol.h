#ifndef VPSDEMO_REGISTRY_PROTOCOL_H_
#define VPSDEMO_REGISTRY_PROTOCOL_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

// Wire format: [Op:1][SaId:4][PayloadLen:2][Payload:N]
// Response:    [ErrCode:4][PayloadLen:2][Payload:N]

enum class RegistryOp : uint8_t {
    Register = 1,  // payload = service_socket_path
    Get = 2,       // no payload
    Load = 3,      // no payload (supervisor starts if absent)
    Unload = 4,    // no payload (supervisor stops + death notify)
};

struct RegistryRequest {
    RegistryOp op{};
    int32_t sa_id = 0;
    std::string payload;
};

struct RegistryResponse {
    int32_t err_code = 0;
    std::string payload;  // service_socket_path for Get/Load
};

// Encode/decode for the wire. Returns false if buffer is too small or malformed.
bool EncodeRegistryRequest(const RegistryRequest& req, std::string* out);
bool DecodeRegistryRequest(const uint8_t* data, size_t len, RegistryRequest* req);

bool EncodeRegistryResponse(const RegistryResponse& resp, std::string* out);
bool DecodeRegistryResponse(const uint8_t* data, size_t len, RegistryResponse* resp);

// Blocking helpers for socket I/O. Return false on disconnect/error.
bool SendMessage(int fd, const std::string& msg);
bool RecvMessage(int fd, std::string* msg);

}  // namespace vpsdemo

#endif  // VPSDEMO_REGISTRY_PROTOCOL_H_
