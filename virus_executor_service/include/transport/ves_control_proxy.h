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
    using HealthSnapshotCallback = std::function<void(const VesHeartbeatReply&)>;

    VesControlProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VesControlProxy() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;
    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId);
    void SetHealthSnapshotCallback(HealthSnapshotCallback callback);

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback);

 private:
    void MonitorSocket();
    void StopMonitorThread();
    void CloseSocket();
    void ResetSocketConnection();
    bool SendCommand(int fd, char cmd) const;
    MemRpc::StatusCode HeartbeatWithTimeout(VesHeartbeatReply& reply, int timeoutMs) const;
    MemRpc::StatusCode ReceiveSessionHandles(MemRpc::BootstrapHandles& handles);
    bool IsPeerDisconnected() const;
    void NotifyPeerDisconnected();
    void PublishHealthSnapshot(const VesHeartbeatReply& reply);

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
    uint64_t sessionId_ = 0;
    MemRpc::EngineDeathCallback deathCallback_;
    HealthSnapshotCallback healthSnapshotCallback_;
    std::mutex callbackMutex_;
};

class VesControlChannelAdapter : public MemRpc::IBootstrapChannel {
 public:
    using HealthSnapshotCallback = VesControlProxy::HealthSnapshotCallback;

    explicit VesControlChannelAdapter(std::shared_ptr<VesControlProxy> proxy);
    ~VesControlChannelAdapter() override = default;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
    void SetHealthSnapshotCallback(HealthSnapshotCallback callback);
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override;

 private:
    std::shared_ptr<VesControlProxy> ReloadProxyLocked(bool forceReload);

    std::mutex mutex_;
    std::shared_ptr<VesControlProxy> proxy_;
    HealthSnapshotCallback healthSnapshotCallback_;
    MemRpc::EngineDeathCallback deathCallback_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
