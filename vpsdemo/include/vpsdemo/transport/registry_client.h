#ifndef INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_CLIENT_H_
#define INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_CLIENT_H_

#include <cstdint>
#include <string>

namespace vpsdemo {

class RegistryClient {
 public:
    explicit RegistryClient(const std::string& registrySocketPath);

    std::string GetServicePath(int32_t sa_id);
    std::string LoadServicePath(int32_t sa_id);
    bool UnloadService(int32_t sa_id);
    bool RegisterService(int32_t sa_id, const std::string& serviceSocketPath);

 private:
    std::string SendRequest(uint8_t op, int32_t sa_id, const std::string& payload = {});

    std::string registry_socket_path_;
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_CLIENT_H_
