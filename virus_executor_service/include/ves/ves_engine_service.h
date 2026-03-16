#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/ves_control_interface.h"
#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"
#include "ves/ves_protocol.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

class VesEventPublisher;

struct VesHealthSnapshot {
    uint32_t status = static_cast<uint32_t>(VesHeartbeatStatus::UnhealthyInternalError);
    uint32_t reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::InternalError);
    uint32_t flags = 0;
    uint32_t inFlight = 0;
    uint32_t lastTaskAgeMs = 0;
    std::string currentTask = "idle";
};

class VesEngineService : public RpcHandlerRegistrar {
 public:
    static constexpr uint32_t LONG_RUNNING_TASK_THRESHOLD_MS = 100;

    void RegisterHandlers(MemRpc::RpcServer* server) override;
    void Initialize();
    bool initialized() const;
    void SetEventPublisher(std::weak_ptr<VesEventPublisher> publisher);

    ScanFileReply ScanFile(const ScanTask& request);
    VesHealthSnapshot GetHealthSnapshot() const;
    MemRpc::StatusCode PublishEvent(uint32_t eventType,
                                    const std::vector<uint8_t>& payload,
                                    uint32_t flags = 0,
                                    uint32_t eventDomain = VES_EVENT_DOMAIN_RUNTIME) const;
    MemRpc::StatusCode PublishTextEvent(uint32_t eventType,
                                        const std::string& payload,
                                        uint32_t flags = 0,
                                        uint32_t eventDomain = VES_EVENT_DOMAIN_RUNTIME) const;

 private:
    struct ActiveTask {
        uint32_t startMonoMs = 0;
    };

    uint64_t AddActiveTask();
    void RemoveActiveTask(uint64_t taskId);

    std::atomic<bool> initialized_{false};
    mutable std::mutex healthMutex_;
    mutable std::mutex eventPublisherMutex_;
    std::weak_ptr<VesEventPublisher> eventPublisher_;
    uint64_t nextTaskId_ = 1;
    std::unordered_map<uint64_t, ActiveTask> activeTasks_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
