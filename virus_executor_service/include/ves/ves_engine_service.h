#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

class VesEventPublisher;

class VesEngineService : public RpcHandlerRegistrar {
 public:
    void RegisterHandlers(AnyCallHandlerSink* sink) override;
    void RegisterHandlers(MemRpc::RpcServer* server);
    void Initialize();
    bool initialized() const;
    void SetEventPublisher(std::weak_ptr<VesEventPublisher> publisher);

    ScanFileReply ScanFile(const ScanTask& request) const;
    MemRpc::StatusCode PublishEvent(uint32_t eventType,
                                    const std::vector<uint8_t>& payload,
                                    uint32_t flags = 0,
                                    uint32_t eventDomain = VES_EVENT_DOMAIN_RUNTIME) const;
    MemRpc::StatusCode PublishTextEvent(uint32_t eventType,
                                        const std::string& payload,
                                        uint32_t flags = 0,
                                        uint32_t eventDomain = VES_EVENT_DOMAIN_RUNTIME) const;

 private:
    std::atomic<bool> initialized_{false};
    mutable std::mutex eventPublisherMutex_;
    std::weak_ptr<VesEventPublisher> eventPublisher_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
