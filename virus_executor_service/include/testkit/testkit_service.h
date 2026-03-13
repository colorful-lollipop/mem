#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_

#include "service/rpc_handler_registrar.h"
#include "testkit/testkit_codec.h"

namespace virus_executor_service::testkit {

struct TestkitServiceOptions {
    bool enableFaultInjection = false;
};

class TestkitService : public virus_executor_service::RpcHandlerRegistrar {
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

}  // namespace virus_executor_service::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_
