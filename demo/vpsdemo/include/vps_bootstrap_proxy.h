#ifndef VPSDEMO_VPS_BOOTSTRAP_PROXY_H_
#define VPSDEMO_VPS_BOOTSTRAP_PROXY_H_

#include <atomic>
#include <string>
#include <thread>

#include "iremote_proxy.h"
#include "vps_bootstrap_interface.h"

namespace vpsdemo {

// Client-side proxy that connects to engine SA's service socket,
// exchanges BootstrapHandles via SCM_RIGHTS, and monitors socket
// health for death recipient triggering.
class VpsBootstrapProxy : public OHOS::IRemoteProxy<IVpsBootstrap> {
 public:
    VpsBootstrapProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                      const std::string& serviceSocketPath);
    ~VpsBootstrapProxy() override;

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) override;
    memrpc::StatusCode CloseSession() override;

 private:
    void MonitorSocket();

    std::string service_socket_path_;
    int sock_fd_ = -1;
    std::atomic<bool> stop_monitor_{false};
    std::thread monitor_thread_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_BOOTSTRAP_PROXY_H_
