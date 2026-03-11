#include <cstddef>
#include <cstdint>
#include <vector>

#include "apps/minirpc/common/minirpc_codec.h"

using namespace OHOS::Security::VirusProtectionService::MiniRpc;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::vector<uint8_t> bytes(data, data + size);

  EchoRequest echoRequest;
  EchoReply echoReply;
  AddRequest addRequest;
  AddReply addReply;
  SleepRequest sleepRequest;
  SleepReply sleepReply;

  const bool echoOk = DecodeMessage<EchoRequest>(bytes, &echoRequest);
  const bool addOk = DecodeMessage<AddRequest>(bytes, &addRequest);
  const bool sleepOk = DecodeMessage<SleepRequest>(bytes, &sleepRequest);

  DecodeMessage<EchoReply>(bytes, &echoReply);
  DecodeMessage<AddReply>(bytes, &addReply);
  DecodeMessage<SleepReply>(bytes, &sleepReply);

  if (echoOk) {
    std::vector<uint8_t> out;
    EncodeMessage<EchoRequest>(echoRequest, &out);
  }
  if (addOk) {
    std::vector<uint8_t> out;
    EncodeMessage<AddRequest>(addRequest, &out);
  }
  if (sleepOk) {
    std::vector<uint8_t> out;
    EncodeMessage<SleepRequest>(sleepRequest, &out);
  }

  return 0;
}
