#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_INTERNAL_VES_CLIENT_RECOVERY_ACCESS_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_INTERNAL_VES_CLIENT_RECOVERY_ACCESS_H_

#include <utility>

#include "client/ves_client.h"

namespace VirusExecutorService::internal {

class VesClientRecoveryAccess {
public:
    using RecoveryEventCallback = MemRpc::RecoveryEventCallback;

    static void SetRecoveryEventCallback(VesClient& client, RecoveryEventCallback callback)
    {
        client.client_.SetRecoveryEventCallback(std::move(callback));
    }

    [[nodiscard]] static MemRpc::RecoveryRuntimeSnapshot GetRecoveryRuntimeSnapshot(const VesClient& client)
    {
        return client.client_.GetRecoveryRuntimeSnapshot();
    }
};

}  // namespace VirusExecutorService::internal

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_INTERNAL_VES_CLIENT_RECOVERY_ACCESS_H_
