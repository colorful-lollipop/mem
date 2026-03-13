#include <cstddef>
#include <cstdint>
#include <vector>

#include "testkit/testkit_codec.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);

    virus_executor_service::testkit::EchoRequest echoRequest;
    virus_executor_service::testkit::EchoReply echoReply;
    virus_executor_service::testkit::AddRequest addRequest;
    virus_executor_service::testkit::AddReply addReply;
    virus_executor_service::testkit::SleepRequest sleepRequest;
    virus_executor_service::testkit::SleepReply sleepReply;

    const bool echoOk = MemRpc::DecodeMessage<virus_executor_service::testkit::EchoRequest>(bytes, &echoRequest);
    const bool addOk = MemRpc::DecodeMessage<virus_executor_service::testkit::AddRequest>(bytes, &addRequest);
    const bool sleepOk =
        MemRpc::DecodeMessage<virus_executor_service::testkit::SleepRequest>(bytes, &sleepRequest);

    (void)MemRpc::DecodeMessage<virus_executor_service::testkit::EchoReply>(bytes, &echoReply);
    (void)MemRpc::DecodeMessage<virus_executor_service::testkit::AddReply>(bytes, &addReply);
    (void)MemRpc::DecodeMessage<virus_executor_service::testkit::SleepReply>(bytes, &sleepReply);

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
