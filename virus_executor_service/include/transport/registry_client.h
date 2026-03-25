#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_CLIENT_H_

#include <cstdint>
#include <string>

#include "transport/registry_protocol.h"

namespace VirusExecutorService {

class RegistryClient {
public:
    explicit RegistryClient(const std::string& registrySocketPath);

    std::string GetServicePath(int32_t sa_id);
    std::string LoadServicePath(int32_t sa_id);
    bool UnloadService(int32_t sa_id);
    bool RegisterService(int32_t sa_id, const std::string& serviceSocketPath);

private:
    std::string SendRequest(RegistryOp op, int32_t sa_id, const std::string& payload = {});

    std::string registry_socket_path_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_CLIENT_H_
