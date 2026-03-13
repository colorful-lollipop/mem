#ifndef INCLUDE_VPSDEMO_VES_VES_ENGINE_SERVICE_H_
#define INCLUDE_VPSDEMO_VES_VES_ENGINE_SERVICE_H_

#include <mutex>
#include <string>

#include "memrpc/server/rpc_server.h"
#include "vpsdemo/rpc_handler_registrar.h"
#include "vpsdemo/ves/ves_types.h"

namespace vpsdemo {

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

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_VES_VES_ENGINE_SERVICE_H_
