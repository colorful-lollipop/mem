#ifndef VPSDEMO_VES_ENGINE_SERVICE_H_
#define VPSDEMO_VES_ENGINE_SERVICE_H_

#include <mutex>
#include <string>

#include "memrpc/server/rpc_server.h"
#include "ves_types.h"

namespace vpsdemo {

struct VesHealthSnapshot {
    uint32_t in_flight = 0;
    uint32_t last_task_age_ms = 0;
    std::string current_task = "idle";
};

class VesEngineService {
 public:
    void RegisterHandlers(memrpc::RpcServer* server);
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

}  // namespace vpsdemo

#endif  // VPSDEMO_VES_ENGINE_SERVICE_H_
