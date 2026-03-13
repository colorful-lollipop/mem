#ifndef INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_BACKEND_H_
#define INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_BACKEND_H_

#include <string>

#include "isam_backend.h"
#include "vpsdemo/transport/registry_client.h"

namespace vpsdemo {

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

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_BACKEND_H_
