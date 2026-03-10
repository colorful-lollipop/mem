#ifndef APPS_VPS_COMMON_VPS_CODEC_H_
#define APPS_VPS_COMMON_VPS_CODEC_H_

#include <cstddef>
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
bool DecodeScanTask(const uint8_t* bytes, std::size_t size, ScanTask* task);
template <typename BytesLike>
bool DecodeScanTask(const BytesLike& bytes, ScanTask* task) {
  return DecodeScanTask(bytes.data(), bytes.size(), task);
}

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes);
bool DecodeScanResult(const uint8_t* bytes, std::size_t size, ScanResult* result);
template <typename BytesLike>
bool DecodeScanResult(const BytesLike& bytes, ScanResult* result) {
  return DecodeScanResult(bytes.data(), bytes.size(), result);
}

bool EncodeBehaviorScanResult(const BehaviorScanResult& result, std::vector<uint8_t>* bytes);
bool DecodeBehaviorScanResult(const uint8_t* bytes, std::size_t size, BehaviorScanResult* result);
template <typename BytesLike>
bool DecodeBehaviorScanResult(const BytesLike& bytes, BehaviorScanResult* result) {
  return DecodeBehaviorScanResult(bytes.data(), bytes.size(), result);
}

bool EncodeScanBehaviorRequest(const ScanBehaviorRequest& request, std::vector<uint8_t>* bytes);
bool DecodeScanBehaviorRequest(const uint8_t* bytes, std::size_t size, ScanBehaviorRequest* request);
template <typename BytesLike>
bool DecodeScanBehaviorRequest(const BytesLike& bytes, ScanBehaviorRequest* request) {
  return DecodeScanBehaviorRequest(bytes.data(), bytes.size(), request);
}

bool EncodeAccessTokenRequest(const AccessTokenRequest& request, std::vector<uint8_t>* bytes);
bool DecodeAccessTokenRequest(const uint8_t* bytes, std::size_t size, AccessTokenRequest* request);
template <typename BytesLike>
bool DecodeAccessTokenRequest(const BytesLike& bytes, AccessTokenRequest* request) {
  return DecodeAccessTokenRequest(bytes.data(), bytes.size(), request);
}

bool EncodeInt32Result(int32_t value, std::vector<uint8_t>* bytes);
bool DecodeInt32Result(const uint8_t* bytes, std::size_t size, int32_t* value);
template <typename BytesLike>
bool DecodeInt32Result(const BytesLike& bytes, int32_t* value) {
  return DecodeInt32Result(bytes.data(), bytes.size(), value);
}

bool EncodeBoolRequest(bool enabled, std::vector<uint8_t>* bytes);
bool DecodeBoolRequest(const uint8_t* bytes, std::size_t size, bool* enabled);
template <typename BytesLike>
bool DecodeBoolRequest(const BytesLike& bytes, bool* enabled) {
  return DecodeBoolRequest(bytes.data(), bytes.size(), enabled);
}

bool EncodePollBehaviorEventReply(const PollBehaviorEventReply& reply, std::vector<uint8_t>* bytes);
bool DecodePollBehaviorEventReply(const uint8_t* bytes,
                                  std::size_t size,
                                  PollBehaviorEventReply* reply);
template <typename BytesLike>
bool DecodePollBehaviorEventReply(const BytesLike& bytes, PollBehaviorEventReply* reply) {
  return DecodePollBehaviorEventReply(bytes.data(), bytes.size(), reply);
}

bool EncodeScanFileReply(int32_t resultCode,
                         const ScanResult& scanResult,
                         std::vector<uint8_t>* bytes);
bool DecodeScanFileReply(const uint8_t* bytes,
                         std::size_t size,
                         int32_t* resultCode,
                         ScanResult* scanResult);
template <typename BytesLike>
bool DecodeScanFileReply(const BytesLike& bytes, int32_t* resultCode, ScanResult* scanResult) {
  return DecodeScanFileReply(bytes.data(), bytes.size(), resultCode, scanResult);
}

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_COMMON_VPS_CODEC_H_
