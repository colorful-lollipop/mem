#ifndef VPSDEMO_VPSDEMO_SERVICE_H_
#define VPSDEMO_VPSDEMO_SERVICE_H_

#include "memrpc/server/rpc_server.h"

namespace vpsdemo {

class VpsDemoService {
 public:
    void RegisterHandlers(memrpc::RpcServer* server);

 private:
    bool initialized_ = false;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPSDEMO_SERVICE_H_
