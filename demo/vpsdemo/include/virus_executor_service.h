#ifndef VPSDEMO_VPS_BOOTSTRAP_STUB_H_
#define VPSDEMO_VPS_BOOTSTRAP_STUB_H_

#include <atomic>
#include <string>
#include <thread>

#include "iremote_stub.h"
#include "system_ability.h"
#include "vps_bootstrap_interface.h"

namespace vpsdemo {

// Engine-side stub: listens on a Unix domain socket and sends
// BootstrapHandles (6 fds + metadata) via SCM_RIGHTS to clients.
class VpsBootstrapStub : public OHOS::SystemAbility,
                          public OHOS::IRemoteStub<IVpsBootstrap> {
 public:
    VpsBootstrapStub();
    ~VpsBootstrapStub() override;

    // Start listening on a socket, return the path.
    bool StartServiceSocket(const std::string& path);
    void StopServiceSocket();
    const std::string& service_socket_path() const { return socket_path_; }

    // Set the handles to distribute to connecting clients.
    void SetBootstrapHandles(const memrpc::BootstrapHandles& handles);

    // IVpsBootstrap (not used directly; handle exchange is via socket).
    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) override;
    memrpc::StatusCode CloseSession() override;

    // SystemAbility lifecycle.
    void OnStart() override;
    void OnStop() override;

 private:
    void AcceptLoop();

    std::string socket_path_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    memrpc::BootstrapHandles handles_{};
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_BOOTSTRAP_STUB_H_
