#ifndef VPSDEMO_VPS_CLIENT_H_
#define VPSDEMO_VPS_CLIENT_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "vps_bootstrap_proxy.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

// Application-level client that owns the full connection to the engine:
// IRemoteObject + VpsBootstrapProxy + RpcClient.
// Provides typed VPS business methods.
class VpsClient {
 public:
    explicit VpsClient(const OHOS::sptr<OHOS::IRemoteObject>& remote);
    ~VpsClient();

    VpsClient(const VpsClient&) = delete;
    VpsClient& operator=(const VpsClient&) = delete;

    // Register VpsBootstrapProxy factory with BrokerRegistration.
    // Call once before using iface_cast<IVpsBootstrap>.
    static void RegisterProxyFactory();

    memrpc::StatusCode Init();
    void Shutdown();

    // Set a callback invoked inside the engine death handler, before restart.
    // Use this to respawn the engine process so the new session can connect.
    using EngineRestartCallback = std::function<void()>;
    void SetEngineRestartCallback(EngineRestartCallback callback);

    memrpc::StatusCode ScanFile(const std::string& path, ScanFileReply* reply);

    // Returns true after the engine process has died.
    bool EngineDied() const;

 private:
    OHOS::sptr<OHOS::IRemoteObject> remote_;
    std::shared_ptr<VpsBootstrapProxy> proxy_;
    memrpc::RpcClient client_;
    std::atomic<bool> engine_died_{false};
    EngineRestartCallback restart_callback_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_CLIENT_H_
