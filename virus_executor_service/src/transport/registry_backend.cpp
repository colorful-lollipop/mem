#include "virus_executor_service/transport/registry_backend.h"

namespace virus_executor_service {

RegistryBackend::RegistryBackend(const std::string& registrySocketPath)
    : client_(registrySocketPath) {}

std::string RegistryBackend::GetServicePath(int32_t saId) {
    return client_.GetServicePath(saId);
}

std::string RegistryBackend::LoadService(int32_t saId) {
    return client_.LoadServicePath(saId);
}

bool RegistryBackend::AddService(int32_t saId, const std::string& servicePath) {
    return client_.RegisterService(saId, servicePath);
}

bool RegistryBackend::UnloadService(int32_t saId) {
    return client_.UnloadService(saId);
}

}  // namespace virus_executor_service
