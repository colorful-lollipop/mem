#ifndef APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_
#define APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_

#include <vector>

#include "apps/minirpc/common/minirpc_types.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

bool EncodeEchoRequest(const EchoRequest& request, std::vector<uint8_t>* bytes);
bool DecodeEchoRequest(const std::vector<uint8_t>& bytes, EchoRequest* request);

bool EncodeEchoReply(const EchoReply& reply, std::vector<uint8_t>* bytes);
bool DecodeEchoReply(const std::vector<uint8_t>& bytes, EchoReply* reply);

bool EncodeAddRequest(const AddRequest& request, std::vector<uint8_t>* bytes);
bool DecodeAddRequest(const std::vector<uint8_t>& bytes, AddRequest* request);

bool EncodeAddReply(const AddReply& reply, std::vector<uint8_t>* bytes);
bool DecodeAddReply(const std::vector<uint8_t>& bytes, AddReply* reply);

bool EncodeSleepRequest(const SleepRequest& request, std::vector<uint8_t>* bytes);
bool DecodeSleepRequest(const std::vector<uint8_t>& bytes, SleepRequest* request);

bool EncodeSleepReply(const SleepReply& reply, std::vector<uint8_t>* bytes);
bool DecodeSleepReply(const std::vector<uint8_t>& bytes, SleepReply* reply);

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_
