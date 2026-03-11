#ifndef VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_
#define VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_

#include "iremote_broker.h"
#include "memrpc/core/bootstrap.h"

namespace vpsdemo {

// SA ID for the VPS bootstrap service.
constexpr int32_t kVpsBootstrapSaId = 1251;

// Interface exposed by the engine SA for session establishment.
class IVpsBootstrap : public OHOS::IRemoteBroker {
 public:
    ~IVpsBootstrap() override = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_BOOTSTRAP_INTERFACE_H_
