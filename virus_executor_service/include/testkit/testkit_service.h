#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_

#include "service/rpc_handler_registrar.h"
#include "testkit/testkit_codec.h"

namespace VirusExecutorService::testkit {

struct TestkitServiceOptions {
    bool enableFaultInjection = false;
};

class TestkitService : public VirusExecutorService::RpcHandlerRegistrar {
 public:
    explicit TestkitService(TestkitServiceOptions options = {});

    void SetFaultInjectionEnabled(bool enabled);
    void RegisterHandlers(MemRpc::RpcServer* server) override;

    [[nodiscard]] EchoReply Echo(const EchoRequest& request) const;
    [[nodiscard]] AddReply Add(const AddRequest& request) const;
    [[nodiscard]] SleepReply Sleep(const SleepRequest& request) const;

 private:
    TestkitServiceOptions options_;
};

}  // namespace VirusExecutorService::testkit

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_SERVICE_H_
