#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "iremote_proxy.h"
#include "memrpc/core/bootstrap.h"
#include "transport/ves_control_interface.h"

namespace VirusExecutorService {

class VesControlProxy : public OHOS::IRemoteProxy<IVesControl> {
 public:
    VesControlProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VesControlProxy() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;
    MemRpc::StatusCode AnyCall(const VesAnyCallRequest& request, VesAnyCallReply& reply) override;
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback);

 private:
    void MonitorSocket();
    void StopMonitorThread();
    void ResetSocketConnection();
    bool SendCommand(int fd, char cmd, const std::vector<uint8_t>& payload = {}) const;
    MemRpc::StatusCode HeartbeatWithTimeout(VesHeartbeatReply& reply, int timeoutMs) const;
    MemRpc::StatusCode ReceiveSizedReply(int fd, std::vector<uint8_t>* payload, int timeoutMs) const;
    MemRpc::StatusCode ReceiveSessionHandles(int fd, MemRpc::BootstrapHandles& handles);
    bool IsPeerDisconnected(int fd) const;
    void NotifyPeerDisconnected();

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
    uint64_t sessionId_ = 0;
    MemRpc::EngineDeathCallback deathCallback_;
    std::mutex operationMutex_;
    std::mutex connectionMutex_;
    std::mutex callbackMutex_;
};

class VesBootstrapChannel : public MemRpc::IBootstrapChannel {
 public:
    using HealthSnapshotCallback = std::function<void(const VesHeartbeatReply&)>;
    using ControlLoader = std::function<OHOS::sptr<IVesControl>(bool forceReload)>;

    explicit VesBootstrapChannel(OHOS::sptr<IVesControl> control,
                                 ControlLoader controlLoader = {});
    ~VesBootstrapChannel() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
    [[nodiscard]] OHOS::sptr<IVesControl> CurrentControl();
    void SetHealthSnapshotCallback(HealthSnapshotCallback callback);
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override;

 private:
    OHOS::sptr<IVesControl> ReloadControlLocked(bool forceReload);
    void RebindControlLocked(const OHOS::sptr<IVesControl>& nextControl);
    void PublishHealthSnapshot(const VesHeartbeatReply& reply);
    void NotifyEngineDeath(uint64_t sessionId);

    std::mutex mutex_;
    OHOS::sptr<IVesControl> control_;
    ControlLoader controlLoader_;
    OHOS::sptr<OHOS::IRemoteObject::DeathRecipient> deathRecipient_;
    HealthSnapshotCallback healthSnapshotCallback_;
    MemRpc::EngineDeathCallback deathCallback_;
    uint64_t sessionId_ = 0;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
