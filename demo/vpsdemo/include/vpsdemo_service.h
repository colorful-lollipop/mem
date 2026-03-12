#ifndef VPSDEMO_VPSDEMO_SERVICE_H_
#define VPSDEMO_VPSDEMO_SERVICE_H_

#include <mutex>
#include <string>

#include "memrpc/server/rpc_server.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

struct VpsHealthSnapshot {
    uint32_t in_flight = 0;
    uint32_t last_task_age_ms = 0;
    std::string current_task = "idle";
};

class VpsDemoService {
 public:
    void RegisterHandlers(memrpc::RpcServer* server);
    void Initialize();
    bool initialized() const;

    ScanFileReply ScanFile(const ScanFileRequest& request);
    VpsHealthSnapshot GetHealthSnapshot() const;

 private:
    bool initialized_ = false;
    mutable std::mutex health_mutex_;
    uint32_t in_flight_ = 0;
    uint32_t last_task_start_mono_ms_ = 0;
    std::string current_task_ = "idle";
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_SERVICE_H_
