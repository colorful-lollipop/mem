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

// 这个类后续会被生产的代码替换，逻辑保持最薄，只是简单的proxy抽象
class VesControlProxy : public OHOS::IRemoteProxy<IVirusProtectionExecutor> {
public:
    VesControlProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote, const std::string& serviceSocketPath);
    ~VesControlProxy() override;

    MemRpc::StatusCode OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles) override;
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
    using ControlLoader = std::function<OHOS::sptr<IVirusProtectionExecutor>()>;
    using AccessPolicy = std::function<bool()>;

    explicit VesBootstrapChannel(ControlLoader controlLoader,
                                 VesOpenSessionRequest openSessionRequest = DefaultVesOpenSessionRequest(),
                                 AccessPolicy accessPolicy = {});
    ~VesBootstrapChannel() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::ChannelHealthResult CheckHealth(uint64_t expectedSessionId) override;
    [[nodiscard]] OHOS::sptr<IVirusProtectionExecutor> CurrentControl();
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override;

private:
    struct State;

    std::shared_ptr<State> state_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
