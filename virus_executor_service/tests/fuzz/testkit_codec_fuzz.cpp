#include <cstddef>
#include <cstdint>
#include <vector>

#include "testkit/testkit_codec.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> bytes(data, data + size);

    VirusExecutorService::testkit::EchoRequest echoRequest;
    VirusExecutorService::testkit::EchoReply echoReply;
    VirusExecutorService::testkit::AddRequest addRequest;
    VirusExecutorService::testkit::AddReply addReply;
    VirusExecutorService::testkit::SleepRequest sleepRequest;
    VirusExecutorService::testkit::SleepReply sleepReply;

    const bool echoOk = MemRpc::DecodeMessage<VirusExecutorService::testkit::EchoRequest>(bytes, &echoRequest);
    const bool addOk = MemRpc::DecodeMessage<VirusExecutorService::testkit::AddRequest>(bytes, &addRequest);
    const bool sleepOk = MemRpc::DecodeMessage<VirusExecutorService::testkit::SleepRequest>(bytes, &sleepRequest);

    (void)MemRpc::DecodeMessage<VirusExecutorService::testkit::EchoReply>(bytes, &echoReply);
    (void)MemRpc::DecodeMessage<VirusExecutorService::testkit::AddReply>(bytes, &addReply);
    (void)MemRpc::DecodeMessage<VirusExecutorService::testkit::SleepReply>(bytes, &sleepReply);

    if (echoOk) {
        std::vector<uint8_t> out;
        (void)MemRpc::EncodeMessage(echoRequest, &out);
    }
    if (addOk) {
        std::vector<uint8_t> out;
        (void)MemRpc::EncodeMessage(addRequest, &out);
    }
    if (sleepOk) {
        std::vector<uint8_t> out;
        (void)MemRpc::EncodeMessage(sleepRequest, &out);
    }

    return 0;
}
