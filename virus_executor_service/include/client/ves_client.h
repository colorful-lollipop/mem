#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "memrpc/client/rpc_client.h"
#include "transport/ves_control_proxy.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

namespace internal {
class VesClientRecoveryAccess;
}

struct VesClientOptions {
    uint32_t execTimeoutRestartDelayMs = 200;
    uint32_t engineDeathRestartDelayMs = 200;
    uint32_t idleShutdownTimeoutMs = 60000;
    VesOpenSessionRequest openSessionRequest;
};

struct VesClientConnectOptions {
    int32_t loadTimeoutMs = 5000;
};

class VesClient {
public:
    using ControlLoader = VesBootstrapChannel::ControlLoader;
    using EventCallback = MemRpc::RpcEventCallback;

    explicit VesClient(ControlLoader controlLoader, VesClientOptions options = {});
    ~VesClient();

    VesClient(const VesClient&) = delete;
    VesClient& operator=(const VesClient&) = delete;

    static void RegisterProxyFactory();
    static std::unique_ptr<VesClient> Connect(VesClientOptions options = {},
                                              VesClientConnectOptions connectOptions = {});

    MemRpc::StatusCode Init();
    void SetEventCallback(EventCallback callback);
    void Shutdown();

    MemRpc::StatusCode ScanFile(const ScanTask& scanTask,
                                ScanFileReply* reply,
                                MemRpc::Priority priority = MemRpc::Priority::Normal,
                                uint32_t execTimeoutMs = 30000);

private:
    template <typename Request, typename Reply>
    MemRpc::StatusCode InvokeApi(MemRpc::Opcode opcode,
                                 const Request& request,
                                 Reply* reply,
                                 MemRpc::Priority priority,
                                 uint32_t execTimeoutMs);

    [[nodiscard]] OHOS::sptr<IVirusProtectionExecutor> CurrentControl();
    friend class internal::VesClientRecoveryAccess;
    ControlLoader controlLoader_;
    std::shared_ptr<VesBootstrapChannel> bootstrapChannel_;
    MemRpc::RpcClient client_;
    VesClientOptions options_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_CLIENT_VES_CLIENT_H_
