#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_BACKEND_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_BACKEND_H_

#include <string>

#include "isam_backend.h"
#include "transport/registry_client.h"

namespace virus_executor_service {

class RegistryBackend : public OHOS::ISamBackend {
 public:
    explicit RegistryBackend(const std::string& registrySocketPath);

    std::string GetServicePath(int32_t saId) override;
    std::string LoadService(int32_t saId) override;
    bool AddService(int32_t saId, const std::string& servicePath) override;
    bool UnloadService(int32_t saId) override;

 private:
    RegistryClient client_;
};

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_BACKEND_H_
