#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"
#include "ves/ves_types.h"

namespace VirusExecutorService {

struct VesHealthSnapshot {
    uint32_t inFlight = 0;
    uint32_t lastTaskAgeMs = 0;
    std::string currentTask = "idle";
};

class VesEngineService : public RpcHandlerRegistrar {
 public:
    void RegisterHandlers(MemRpc::RpcServer* server) override;
    void Initialize();
    bool initialized() const;

    ScanFileReply ScanFile(const ScanFileRequest& request);
    VesHealthSnapshot GetHealthSnapshot() const;

 private:
    struct ActiveTask {
        uint32_t startMonoMs = 0;
        std::string filePath;
    };

    uint64_t AddActiveTask(const std::string& filePath);
    void RemoveActiveTask(uint64_t taskId);

    bool initialized_ = false;
    mutable std::mutex healthMutex_;
    uint64_t nextTaskId_ = 1;
    std::unordered_map<uint64_t, ActiveTask> activeTasks_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
