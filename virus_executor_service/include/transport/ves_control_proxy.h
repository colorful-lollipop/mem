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

class VesControlProxy : public OHOS::IRemoteProxy<IVirusProtectionExecutorS> {
 public:
    VesControlProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VesControlProxy() override;

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request,
                                   MemRpc::BootstrapHandles& handles) override;
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
    using ControlLoader = std::function<OHOS::sptr<IVirusProtectionExecutorS>()>;

    explicit VesBootstrapChannel(const OHOS::sptr<IVirusProtectionExecutorS>& control,
                                 VesOpenSessionRequest openSessionRequest = DefaultVesOpenSessionRequest(),
                                 ControlLoader controlLoader = {});
    ~VesBootstrapChannel() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
    [[nodiscard]] OHOS::sptr<IVirusProtectionExecutorS> CurrentControl();
    void SetHealthSnapshotCallback(HealthSnapshotCallback callback);
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override;

 private:
    struct DeathRecipientContext;
    struct HealthSnapshotContext;

    OHOS::sptr<IVirusProtectionExecutorS> EnsureControlBoundLocked();
    OHOS::sptr<IVirusProtectionExecutorS> RefreshControlLocked();
    void RebindControlLocked(const OHOS::sptr<IVirusProtectionExecutorS>& nextControl);
    void PublishHealthSnapshot(const VesHeartbeatReply& reply);
    void NotifyEngineDeath(uint64_t sessionId);
    void HandleRemoteDied();
    void ShutdownDeathRecipient();
    void InvalidateHealthSnapshotCallbacks(bool shuttingDown);

    std::mutex mutex_;
    OHOS::sptr<IVirusProtectionExecutorS> control_;
    ControlLoader controlLoader_;
    std::shared_ptr<DeathRecipientContext> deathRecipientContext_;
    std::shared_ptr<HealthSnapshotContext> healthSnapshotContext_;
    OHOS::sptr<OHOS::IRemoteObject::DeathRecipient> deathRecipient_;
    HealthSnapshotCallback healthSnapshotCallback_;
    MemRpc::EngineDeathCallback deathCallback_;
    uint64_t sessionId_ = 0;
    VesOpenSessionRequest openSessionRequest_{};
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
