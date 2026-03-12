#ifndef VPSDEMO_VPS_BOOTSTRAP_PROXY_H_
#define VPSDEMO_VPS_BOOTSTRAP_PROXY_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "iremote_proxy.h"
#include "memrpc/core/bootstrap.h"
#include "vps_bootstrap_interface.h"

namespace vpsdemo {

// Client-side proxy that connects to engine SA's service socket,
// exchanges BootstrapHandles via SCM_RIGHTS, and monitors socket
// health for death detection.
//
// Implements both the OHOS IRemoteProxy interface (for SA compatibility)
// and memrpc::IBootstrapChannel (so RpcClient can use it directly and
// receive engine death notifications through the framework path).
class VpsBootstrapProxy : public OHOS::IRemoteProxy<IVpsBootstrap>,
                          public memrpc::IBootstrapChannel {
 public:
    VpsBootstrapProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VpsBootstrapProxy() override;

    // Shared by both IVpsBootstrap and IBootstrapChannel.
    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles& handles) override;
    memrpc::StatusCode CloseSession() override;
    memrpc::StatusCode Heartbeat(VpsHeartbeatReply& reply) override;

    // IBootstrapChannel — framework death callback.
    void SetEngineDeathCallback(memrpc::EngineDeathCallback callback) override;

 private:
    void MonitorSocket();

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
    uint64_t session_id_ = 0;
    memrpc::EngineDeathCallback death_callback_;
    std::mutex callback_mutex_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_BOOTSTRAP_PROXY_H_
