#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"

namespace VirusExecutorService {

class VesSessionProvider {
public:
    virtual ~VesSessionProvider() = default;
    virtual MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) = 0;
    virtual MemRpc::StatusCode CloseSession() = 0;
};

class VesEventPublisher {
public:
    virtual ~VesEventPublisher() = default;
    virtual MemRpc::StatusCode PublishEventBlocking(const MemRpc::RpcEvent& event) = 0;
};

class EngineSessionService final : public VesSessionProvider, public VesEventPublisher {
public:
    explicit EngineSessionService(std::vector<RpcHandlerRegistrar*> registrars = {});
    ~EngineSessionService() override;

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override;
    MemRpc::StatusCode CloseSession() override;
    MemRpc::StatusCode PublishEventBlocking(const MemRpc::RpcEvent& event) override;
    MemRpc::StatusCode InvokeAnyCall(const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply);

    [[nodiscard]] uint64_t session_id() const;
    [[nodiscard]] MemRpc::RpcServerRuntimeStats GetRuntimeStats() const;

private:
    MemRpc::StatusCode EnsureInitialized();
    void StartEventPublisherLocked();
    void EventPublisherLoop();

    std::vector<RpcHandlerRegistrar*> registrars_;
    std::unordered_map<uint16_t, MemRpc::RpcHandler> anyCallHandlers_;
    std::shared_ptr<MemRpc::DevBootstrapChannel> bootstrap_;
    std::shared_ptr<MemRpc::RpcServer> rpcServer_;
    mutable std::mutex initMutex_;
    bool initialized_ = false;
    bool closing_ = false;
    std::atomic<uint64_t> sessionId_{0};
    std::atomic<bool> eventPublisherRunning_{false};
    std::thread eventPublisherThread_;
    std::mutex eventPublisherMutex_;
    std::condition_variable eventPublisherCv_;
    std::atomic<uint32_t> eventSequence_{0};
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_SESSION_SERVICE_H_
