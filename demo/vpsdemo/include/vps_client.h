#ifndef VPSDEMO_VPS_CLIENT_H_
#define VPSDEMO_VPS_CLIENT_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "vps_bootstrap_proxy.h"
#include "ves_types.h"

namespace vpsdemo {

struct VesClientOptions {
    uint32_t execTimeoutRestartDelayMs = 200;
    uint32_t engineDeathRestartDelayMs = 200;
    uint32_t idleRestartDelayMs = 0;
};

// Application-level client that owns the full connection to the engine:
// IRemoteObject + VesBootstrapProxy + RpcClient.
// Provides typed VPS business methods.
class VesClient {
 public:
    explicit VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                       VesClientOptions options = {});
    ~VesClient();

    VesClient(const VesClient&) = delete;
    VesClient& operator=(const VesClient&) = delete;

    // Register VesBootstrapProxy factory with BrokerRegistration.
    // Call once before using iface_cast<IVesBootstrap>.
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
    std::shared_ptr<VesBootstrapProxy> proxy_;
    memrpc::RpcClient client_;
    VesClientOptions options_;
    std::atomic<bool> engineDied_{false};
    EngineRestartCallback restartCallback_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_CLIENT_H_
