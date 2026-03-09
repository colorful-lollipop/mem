#ifndef APPS_VPS_COMMON_VPS_CODEC_H_
#define APPS_VPS_COMMON_VPS_CODEC_H_

#include <cstdint>
#include <vector>

#include "apps/vps/common/virus_protection_service_define.h"

namespace OHOS::Security::VirusProtectionService {

struct ScanBehaviorRequest {
  uint32_t accessToken = 0;
  std::string event;
  std::string bundleName;
};

struct AccessTokenRequest {
  uint32_t accessToken = 0;
};

struct PollBehaviorEventReply {
  int32_t result = NOT_FOUND;
  uint32_t accessToken = 0;
  BehaviorScanResult scanResult;
};

bool EncodeScanTask(const ScanTask& task, std::vector<uint8_t>* bytes);
bool DecodeScanTask(const std::vector<uint8_t>& bytes, ScanTask* task);

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes);
bool DecodeScanResult(const std::vector<uint8_t>& bytes, ScanResult* result);

bool EncodeBehaviorScanResult(const BehaviorScanResult& result, std::vector<uint8_t>* bytes);
bool DecodeBehaviorScanResult(const std::vector<uint8_t>& bytes, BehaviorScanResult* result);

bool EncodeScanBehaviorRequest(const ScanBehaviorRequest& request, std::vector<uint8_t>* bytes);
bool DecodeScanBehaviorRequest(const std::vector<uint8_t>& bytes, ScanBehaviorRequest* request);

bool EncodeAccessTokenRequest(const AccessTokenRequest& request, std::vector<uint8_t>* bytes);
bool DecodeAccessTokenRequest(const std::vector<uint8_t>& bytes, AccessTokenRequest* request);

bool EncodeInt32Result(int32_t value, std::vector<uint8_t>* bytes);
bool DecodeInt32Result(const std::vector<uint8_t>& bytes, int32_t* value);

bool EncodeBoolRequest(bool enabled, std::vector<uint8_t>* bytes);
bool DecodeBoolRequest(const std::vector<uint8_t>& bytes, bool* enabled);

bool EncodePollBehaviorEventReply(const PollBehaviorEventReply& reply, std::vector<uint8_t>* bytes);
bool DecodePollBehaviorEventReply(const std::vector<uint8_t>& bytes, PollBehaviorEventReply* reply);

bool EncodeScanFileReply(int32_t resultCode,
                         const ScanResult& scanResult,
                         std::vector<uint8_t>* bytes);
bool DecodeScanFileReply(const std::vector<uint8_t>& bytes,
                         int32_t* resultCode,
                         ScanResult* scanResult);

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_COMMON_VPS_CODEC_H_
