#ifndef VPSDEMO_TESTKIT_SERVICE_H_
#define VPSDEMO_TESTKIT_SERVICE_H_

#include "rpc_handler_registrar.h"
#include "testkit_codec.h"

namespace vpsdemo::testkit {

struct TestkitServiceOptions {
    bool enableFaultInjection = false;
};

class TestkitService : public vpsdemo::RpcHandlerRegistrar {
 public:
    explicit TestkitService(TestkitServiceOptions options = {});

    void SetFaultInjectionEnabled(bool enabled);
    void RegisterHandlers(memrpc::RpcServer* server) override;

    EchoReply Echo(const EchoRequest& request) const;
    AddReply Add(const AddRequest& request) const;
    SleepReply Sleep(const SleepRequest& request) const;

 private:
    TestkitServiceOptions options_;
};

}  // namespace vpsdemo::testkit

#endif  // VPSDEMO_TESTKIT_SERVICE_H_
