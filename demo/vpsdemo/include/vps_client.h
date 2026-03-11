#ifndef VPSDEMO_VPS_CLIENT_H_
#define VPSDEMO_VPS_CLIENT_H_

#include <atomic>
#include <memory>
#include <string>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "vps_bootstrap_proxy.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

// Application-level client that owns the full connection to the engine:
// IRemoteObject + VpsBootstrapProxy + RpcClient.
// Provides typed VPS business methods.  Internally registers an OHOS
// DeathRecipient to track engine liveness.
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

    memrpc::StatusCode InitEngine(InitReply* reply);
    memrpc::StatusCode ScanFile(const std::string& path, ScanFileReply* reply);
    memrpc::StatusCode UpdateFeatureLib(UpdateFeatureLibReply* reply);

    // Returns true after the engine process has died.
    bool engine_died() const;

 private:
    class DeathRecipientImpl;

    OHOS::sptr<OHOS::IRemoteObject> remote_;
    std::shared_ptr<VpsBootstrapProxy> proxy_;
    memrpc::RpcClient client_;
    OHOS::sptr<DeathRecipientImpl> death_recipient_;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_CLIENT_H_
