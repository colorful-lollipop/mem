#ifndef INCLUDE_VPSDEMO_TRANSPORT_VES_BOOTSTRAP_PROXY_H_
#define INCLUDE_VPSDEMO_TRANSPORT_VES_BOOTSTRAP_PROXY_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "iremote_proxy.h"
#include "memrpc/core/bootstrap.h"
#include "vpsdemo/transport/ves_bootstrap_interface.h"

namespace vpsdemo {

class VesBootstrapProxy : public OHOS::IRemoteProxy<IVesBootstrap>,
                          public memrpc::IBootstrapChannel {
 public:
    VesBootstrapProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VesBootstrapProxy() override;

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;
    memrpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;

    void SetEngineDeathCallback(memrpc::EngineDeathCallback callback) override;

 private:
    void MonitorSocket();

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
    uint64_t sessionId_ = 0;
    memrpc::EngineDeathCallback deathCallback_;
    std::mutex callbackMutex_;
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_TRANSPORT_VES_BOOTSTRAP_PROXY_H_
