#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_

#include <mutex>
#include <string>

#include "memrpc/server/rpc_server.h"
#include "service/rpc_handler_registrar.h"
#include "ves/ves_types.h"

namespace virus_executor_service {

struct VesHealthSnapshot {
    uint32_t inFlight = 0;
    uint32_t lastTaskAgeMs = 0;
    std::string currentTask = "idle";
};

class VesEngineService : public RpcHandlerRegistrar {
 public:
    void RegisterHandlers(memrpc::RpcServer* server) override;
    void Initialize();
    bool initialized() const;

    ScanFileReply ScanFile(const ScanFileRequest& request);
    VesHealthSnapshot GetHealthSnapshot() const;

 private:
    bool initialized_ = false;
    mutable std::mutex healthMutex_;
    uint32_t inFlight_ = 0;
    uint32_t lastTaskStartMonoMs_ = 0;
    std::string currentTask_ = "idle";
};

}  // namespace virus_executor_service

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_ENGINE_SERVICE_H_
