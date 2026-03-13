#include <cstddef>
#include <cstdint>
#include <vector>

#include "testkit_codec.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> bytes(data, data + size);

    vpsdemo::testkit::EchoRequest echoRequest;
    vpsdemo::testkit::EchoReply echoReply;
    vpsdemo::testkit::AddRequest addRequest;
    vpsdemo::testkit::AddReply addReply;
    vpsdemo::testkit::SleepRequest sleepRequest;
    vpsdemo::testkit::SleepReply sleepReply;

    const bool echoOk = memrpc::DecodeMessage<vpsdemo::testkit::EchoRequest>(bytes, &echoRequest);
    const bool addOk = memrpc::DecodeMessage<vpsdemo::testkit::AddRequest>(bytes, &addRequest);
    const bool sleepOk =
        memrpc::DecodeMessage<vpsdemo::testkit::SleepRequest>(bytes, &sleepRequest);

    (void)memrpc::DecodeMessage<vpsdemo::testkit::EchoReply>(bytes, &echoReply);
    (void)memrpc::DecodeMessage<vpsdemo::testkit::AddReply>(bytes, &addReply);
    (void)memrpc::DecodeMessage<vpsdemo::testkit::SleepReply>(bytes, &sleepReply);

    if (echoOk) {
        std::vector<uint8_t> out;
        (void)memrpc::EncodeMessage(echoRequest, &out);
    }
    if (addOk) {
        std::vector<uint8_t> out;
        (void)memrpc::EncodeMessage(addRequest, &out);
    }
    if (sleepOk) {
        std::vector<uint8_t> out;
        (void)memrpc::EncodeMessage(sleepRequest, &out);
    }

    return 0;
}
