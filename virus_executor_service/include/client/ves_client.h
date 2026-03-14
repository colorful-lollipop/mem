#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "iremote_object.h"
#include "memrpc/client/rpc_client.h"
#include "transport/ves_bootstrap_proxy.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

struct VesClientOptions {
    uint32_t execTimeoutRestartDelayMs = 200;
    uint32_t engineDeathRestartDelayMs = 200;
    uint32_t idleRestartDelayMs = 0;
};

class VesClient {
 public:
    explicit VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                       VesClientOptions options = {});
    ~VesClient();

    VesClient(const VesClient&) = delete;
    VesClient& operator=(const VesClient&) = delete;

    static void RegisterProxyFactory();

    MemRpc::StatusCode Init();
    void Shutdown();

    using EngineRestartCallback = std::function<void()>;
    void SetEngineRestartCallback(EngineRestartCallback callback);

    MemRpc::StatusCode ScanFile(const std::string& path, ScanFileReply* reply);

    bool EngineDied() const;

 private:
    OHOS::sptr<OHOS::IRemoteObject> remote_;
    std::shared_ptr<VesBootstrapProxy> proxy_;
    MemRpc::RpcClient client_;
    VesClientOptions options_;
    std::atomic<bool> engineDied_{false};
    EngineRestartCallback restartCallback_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
