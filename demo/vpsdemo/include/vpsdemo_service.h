#ifndef VPSDEMO_VPSDEMO_SERVICE_H_
#define VPSDEMO_VPSDEMO_SERVICE_H_

#include "memrpc/server/rpc_server.h"
#include "vpsdemo_types.h"

namespace vpsdemo {

class VpsDemoService {
 public:
    void RegisterHandlers(memrpc::RpcServer* server);
    void Initialize();
    bool initialized() const;

    ScanFileReply ScanFile(const ScanFileRequest& request);

 private:
    bool initialized_ = false;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_SERVICE_H_
