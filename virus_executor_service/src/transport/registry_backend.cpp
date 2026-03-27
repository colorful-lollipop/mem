#include "transport/registry_backend.h"

#include "virus_protection_executor_log.h"

namespace VirusExecutorService {

RegistryBackend::RegistryBackend(const std::string& registrySocketPath)
    : client_(registrySocketPath)
{
}

std::string RegistryBackend::GetServicePath(int32_t saId)
{
    std::string path = client_.GetServicePath(saId);
    if (path.empty()) {
        HILOGE("RegistryBackend::GetServicePath failed: sa_id=%{public}d", saId);
    }
    return path;
}

std::string RegistryBackend::LoadService(int32_t saId)
{
    std::string path = client_.LoadServicePath(saId);
    if (path.empty()) {
        HILOGE("RegistryBackend::LoadService failed: sa_id=%{public}d", saId);
    }
    return path;
}

bool RegistryBackend::AddService(int32_t saId, const std::string& servicePath)
{
    const bool ok = client_.RegisterService(saId, servicePath);
    if (!ok) {
        HILOGE("RegistryBackend::AddService failed: sa_id=%{public}d path=%{public}s", saId, servicePath.c_str());
    }
    return ok;
}

bool RegistryBackend::UnloadService(int32_t saId)
{
    const bool ok = client_.UnloadService(saId);
    if (!ok) {
        HILOGE("RegistryBackend::UnloadService failed: sa_id=%{public}d", saId);
    }
    return ok;
}

}  // namespace VirusExecutorService
