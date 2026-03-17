#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "transport/ves_control_proxy.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

struct VesClientOptions {
    uint32_t execTimeoutRestartDelayMs = 200;
    uint32_t engineDeathRestartDelayMs = 200;
    uint32_t idleShutdownTimeoutMs = 0;
};

struct VesClientConnectOptions {
    bool checkExisting = true;
    bool loadIfMissing = true;
    int32_t loadTimeoutMs = 5000;
};

class VesClient {
 public:
    using HealthSnapshotCallback = VesBootstrapChannel::HealthSnapshotCallback;
    using EventCallback = MemRpc::RpcEventCallback;

    explicit VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                       VesClientOptions options = {});
    ~VesClient();

    VesClient(const VesClient&) = delete;
    VesClient& operator=(const VesClient&) = delete;

    static void RegisterProxyFactory();
    static std::unique_ptr<VesClient> Connect(VesClientOptions options = {},
                                              VesClientConnectOptions connectOptions = {});

    MemRpc::StatusCode Init();
    void SetEventCallback(EventCallback callback);
    void SetHealthSnapshotCallback(HealthSnapshotCallback callback);
    void RequestRecovery(uint32_t delayMs = 0);
    void Shutdown();

    MemRpc::StatusCode ScanFile(const ScanTask& scanTask,
                                ScanFileReply* reply,
                                MemRpc::Priority priority = MemRpc::Priority::Normal,
                                uint32_t execTimeoutMs = 30000);

    [[nodiscard]] bool EngineDied() const;
    [[nodiscard]] MemRpc::RecoveryRuntimeSnapshot GetRecoveryRuntimeSnapshot() const;

 private:
    MemRpc::StatusCode InvokeWithRecovery(const std::function<MemRpc::StatusCode()>& invoke);
    void CacheRecoverySnapshot(const MemRpc::RecoveryRuntimeSnapshot& snapshot);
    void CacheRecoveryEvent(const MemRpc::RecoveryEventReport& report);
    bool WaitForRecoveryRetry(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] MemRpc::RecoveryRuntimeSnapshot GetCachedRecoverySnapshot() const;
    [[nodiscard]] std::chrono::milliseconds RecoveryWaitTimeout(
        const MemRpc::RecoveryRuntimeSnapshot& snapshot) const;

    OHOS::sptr<OHOS::IRemoteObject> remote_;
    OHOS::sptr<IVesControl> control_;
    std::shared_ptr<VesBootstrapChannel> bootstrapChannel_;
    MemRpc::RpcClient client_;
    VesClientOptions options_;
    HealthSnapshotCallback healthSnapshotCallback_;
    mutable std::mutex recoveryMutex_;
    std::condition_variable recoveryCv_;
    MemRpc::RecoveryRuntimeSnapshot recoverySnapshot_;
    MemRpc::RecoveryEventReport lastRecoveryEvent_;
    bool hasRecoveryEvent_ = false;
    MemRpc::RecoveryTrigger lastObservedTrigger_ = MemRpc::RecoveryTrigger::Unknown;
    uint64_t recoveryStateVersion_ = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
