#ifndef VPSDEMO_VES_BOOTSTRAP_PROXY_H_
#define VPSDEMO_VES_BOOTSTRAP_PROXY_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "iremote_proxy.h"
#include "memrpc/core/bootstrap.h"
#include "ves_bootstrap_interface.h"

namespace vpsdemo {

// Client-side proxy that connects to engine SA's service socket,
// exchanges BootstrapHandles via SCM_RIGHTS, and monitors socket
// health for death detection.
//
// Implements both the OHOS IRemoteProxy interface (for SA compatibility)
// and memrpc::IBootstrapChannel (so RpcClient can use it directly and
// receive engine death notifications through the framework path).
class VesBootstrapProxy : public OHOS::IRemoteProxy<IVesBootstrap>,
                          public memrpc::IBootstrapChannel {
 public:
    VesBootstrapProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VesBootstrapProxy() override;

    // Shared by both IVesBootstrap and IBootstrapChannel.
    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;
    memrpc::StatusCode Heartbeat(VesHeartbeatReply& reply) override;

    // IBootstrapChannel — framework death callback.
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

#endif  // VPSDEMO_VES_BOOTSTRAP_PROXY_H_
