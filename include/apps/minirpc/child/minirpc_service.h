#ifndef APPS_MINIRPC_CHILD_MINIRPC_SERVICE_H_
#define APPS_MINIRPC_CHILD_MINIRPC_SERVICE_H_

#include "apps/minirpc/common/minirpc_codec.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

class MiniRpcService {
 public:
  MiniRpcService() = default;

  EchoReply Echo(const EchoRequest& request) const;
  AddReply Add(const AddRequest& request) const;
  SleepReply Sleep(const SleepRequest& request) const;

  void RegisterHandlers(MemRpc::RpcServer* server);
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_CHILD_MINIRPC_SERVICE_H_
