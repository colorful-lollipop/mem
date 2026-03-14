#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_

#include <atomic>
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

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback);

 private:
    void MonitorSocket();

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
    uint64_t sessionId_ = 0;
    MemRpc::EngineDeathCallback deathCallback_;
    std::mutex callbackMutex_;
};

class VesControlChannelAdapter : public MemRpc::IBootstrapChannel {
 public:
    explicit VesControlChannelAdapter(std::shared_ptr<VesControlProxy> proxy);
    ~VesControlChannelAdapter() override = default;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override;

 private:
    std::shared_ptr<VesControlProxy> proxy_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_VES_CONTROL_PROXY_H_
