#include "apps/vps/common/vps_codec.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "memrpc/core/byte_reader.h"
#include "memrpc/core/byte_writer.h"
#include "core/protocol.h"
#include "virus_protection_service_log.h"

namespace OHOS::Security::VirusProtectionService {

namespace {

using memrpc::ByteReader;
using memrpc::ByteWriter;

bool WriteUint64(ByteWriter* writer, uint64_t value) {
  return writer != nullptr && writer->WriteBytes(&value, sizeof(value));
}

bool WriteInt64(ByteWriter* writer, int64_t value) {
  return writer != nullptr && writer->WriteBytes(&value, sizeof(value));
}

bool ReadUint64(ByteReader* reader, uint64_t* value) {
  return reader != nullptr && value != nullptr && reader->ReadBytes(value, sizeof(*value));
}

bool ReadInt64(ByteReader* reader, int64_t* value) {
  return reader != nullptr && value != nullptr && reader->ReadBytes(value, sizeof(*value));
}

bool WriteBool(ByteWriter* writer, bool value) {
  return writer != nullptr && writer->WriteUint32(value ? 1u : 0u);
}

bool ReadBool(ByteReader* reader, bool* value) {
  if (reader == nullptr || value == nullptr) {
    return false;
  }
  uint32_t raw = 0;
  if (!reader->ReadUint32(&raw)) {
    return false;
  }
  *value = raw != 0u;
  return true;
}

template <typename EnumType>
bool WriteEnum(ByteWriter* writer, EnumType value) {
  return writer != nullptr && writer->WriteInt32(static_cast<int32_t>(value));
}

template <typename EnumType>
bool ReadEnum(ByteReader* reader, EnumType* value) {
  if (reader == nullptr || value == nullptr) {
    return false;
  }
  int32_t raw = 0;
  if (!reader->ReadInt32(&raw)) {
    return false;
  }
  *value = static_cast<EnumType>(raw);
  return true;
}

bool EncodeFileInfos(const std::vector<std::shared_ptr<BasicFileInfo>>& infos, ByteWriter* writer);
bool DecodeFileInfos(ByteReader* reader, std::vector<std::shared_ptr<BasicFileInfo>>* infos);
bool EncodeEngineResults(const std::vector<EngineResult>& results, ByteWriter* writer);
bool DecodeEngineResults(ByteReader* reader, std::vector<EngineResult>* results);

bool EncodeBundleInfoInternal(const std::shared_ptr<BundleInfo>& info, ByteWriter* writer) {
  if (writer == nullptr) {
    return false;
  }
  if (info == nullptr) {
    return writer->WriteUint32(0u);
  }
  return writer->WriteUint32(1u) && writer->WriteString(info->bundleName) &&
         writer->WriteString(info->appDistributionType) && writer->WriteString(info->versionName) &&
         writer->WriteString(info->label) && WriteBool(writer, info->isEncrypted);
}

bool DecodeBundleInfoInternal(ByteReader* reader, std::shared_ptr<BundleInfo>* info) {
  if (reader == nullptr || info == nullptr) {
    return false;
  }
  uint32_t present = 0;
  if (!reader->ReadUint32(&present)) {
    return false;
  }
  if (present == 0u) {
    info->reset();
    return true;
  }
  auto decoded = std::make_shared<BundleInfo>();
  return reader->ReadString(&decoded->bundleName) &&
         reader->ReadString(&decoded->appDistributionType) &&
         reader->ReadString(&decoded->versionName) && reader->ReadString(&decoded->label) &&
         ReadBool(reader, &decoded->isEncrypted) && ((*info = std::move(decoded)), true);
}

bool EncodeBasicFileInfoInternal(const std::shared_ptr<BasicFileInfo>& info, ByteWriter* writer) {
  if (writer == nullptr) {
    return false;
  }
  if (info == nullptr) {
    return writer->WriteUint32(0u);
  }
  return writer->WriteUint32(1u) && writer->WriteString(info->filePath) &&
         writer->WriteString(info->fileHash) && writer->WriteString(info->subFilePath) &&
         writer->WriteString(info->subFileHash) && WriteInt64(writer, info->inode) &&
         WriteInt64(writer, info->mtime) && WriteUint64(writer, info->fileSize);
}

bool DecodeBasicFileInfoInternal(ByteReader* reader, std::shared_ptr<BasicFileInfo>* info) {
  if (reader == nullptr || info == nullptr) {
    return false;
  }
  uint32_t present = 0;
  if (!reader->ReadUint32(&present)) {
    return false;
  }
  if (present == 0u) {
    info->reset();
    return true;
  }
  auto decoded = std::make_shared<BasicFileInfo>();
  return reader->ReadString(&decoded->filePath) && reader->ReadString(&decoded->fileHash) &&
         reader->ReadString(&decoded->subFilePath) && reader->ReadString(&decoded->subFileHash) &&
         ReadInt64(reader, &decoded->inode) && ReadInt64(reader, &decoded->mtime) &&
         ReadUint64(reader, &decoded->fileSize) && ((*info = std::move(decoded)), true);
}

bool EncodeEngineResultInternal(const EngineResult& result, ByteWriter* writer) {
  return writer != nullptr && writer->WriteString(result.virusName) &&
         writer->WriteString(result.virusType) && writer->WriteString(result.errorMsg) &&
         writer->WriteInt32(result.errorCode) && WriteEnum(writer, result.level);
}

bool DecodeEngineResultInternal(ByteReader* reader, EngineResult* result) {
  return reader != nullptr && result != nullptr && reader->ReadString(&result->virusName) &&
         reader->ReadString(&result->virusType) && reader->ReadString(&result->errorMsg) &&
         reader->ReadInt32(&result->errorCode) && ReadEnum(reader, &result->level);
}

bool EncodeBehaviorScanResultInternal(const BehaviorScanResult& result, ByteWriter* writer) {
  return writer != nullptr && writer->WriteString(result.eventId) && writer->WriteString(result.time) &&
         writer->WriteString(result.ruleName) && writer->WriteString(result.bundleName);
}

bool DecodeBehaviorScanResultInternal(ByteReader* reader, BehaviorScanResult* result) {
  return reader != nullptr && result != nullptr && reader->ReadString(&result->eventId) &&
         reader->ReadString(&result->time) && reader->ReadString(&result->ruleName) &&
         reader->ReadString(&result->bundleName);
}

bool EncodeScanTaskInternal(const ScanTask& task, ByteWriter* writer) {
  return writer != nullptr && writer->WriteString(task.bundleName) &&
         EncodeBundleInfoInternal(task.bundleInfo, writer) &&
         EncodeFileInfos(task.fileInfos, writer) && WriteEnum(writer, task.scanTaskType) &&
         writer->WriteInt32(task.accountId);
}

bool DecodeScanTaskInternal(ByteReader* reader, ScanTask* task) {
  return reader != nullptr && task != nullptr && reader->ReadString(&task->bundleName) &&
         DecodeBundleInfoInternal(reader, &task->bundleInfo) &&
         DecodeFileInfos(reader, &task->fileInfos) &&
         ReadEnum(reader, &task->scanTaskType) && reader->ReadInt32(&task->accountId);
}

bool EncodeScanResultInternal(const ScanResult& result, ByteWriter* writer) {
  return writer != nullptr && writer->WriteString(result.bundleName) &&
         EncodeBundleInfoInternal(result.bundleInfo, writer) &&
         EncodeFileInfos(result.fileInfos, writer) && writer->WriteString(result.bakPath) &&
         WriteEnum(writer, result.threatLevel) && WriteEnum(writer, result.scanTaskType) &&
         writer->WriteInt32(result.accountId) && EncodeEngineResults(result.engineResults, writer);
}

bool DecodeScanResultInternal(ByteReader* reader, ScanResult* result) {
  return reader != nullptr && result != nullptr && reader->ReadString(&result->bundleName) &&
         DecodeBundleInfoInternal(reader, &result->bundleInfo) &&
         DecodeFileInfos(reader, &result->fileInfos) && reader->ReadString(&result->bakPath) &&
         ReadEnum(reader, &result->threatLevel) && ReadEnum(reader, &result->scanTaskType) &&
         reader->ReadInt32(&result->accountId) &&
         DecodeEngineResults(reader, &result->engineResults);
}

bool EncodeFileInfos(const std::vector<std::shared_ptr<BasicFileInfo>>& infos, ByteWriter* writer) {
  if (writer == nullptr || !writer->WriteUint32(static_cast<uint32_t>(infos.size()))) {
    return false;
  }
  for (const auto& info : infos) {
    if (!EncodeBasicFileInfoInternal(info, writer)) {
      return false;
    }
  }
  return true;
}

bool DecodeFileInfos(ByteReader* reader, std::vector<std::shared_ptr<BasicFileInfo>>* infos) {
  if (reader == nullptr || infos == nullptr) {
    return false;
  }
  uint32_t count = 0;
  if (!reader->ReadUint32(&count)) {
    return false;
  }
  infos->clear();
  infos->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::shared_ptr<BasicFileInfo> info;
    if (!DecodeBasicFileInfoInternal(reader, &info)) {
      return false;
    }
    infos->push_back(std::move(info));
  }
  return true;
}

bool EncodeEngineResults(const std::vector<EngineResult>& results, ByteWriter* writer) {
  if (writer == nullptr || !writer->WriteUint32(static_cast<uint32_t>(results.size()))) {
    return false;
  }
  for (const auto& result : results) {
    if (!EncodeEngineResultInternal(result, writer)) {
      return false;
    }
  }
  return true;
}

bool DecodeEngineResults(ByteReader* reader, std::vector<EngineResult>* results) {
  if (reader == nullptr || results == nullptr) {
    return false;
  }
  uint32_t count = 0;
  if (!reader->ReadUint32(&count)) {
    return false;
  }
  results->assign(count, EngineResult{});
  for (uint32_t i = 0; i < count; ++i) {
    if (!DecodeEngineResultInternal(reader, &(*results)[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool EncodeScanTask(const ScanTask& task, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!EncodeScanTaskInternal(task, &writer)) {
    return false;
  }
  *bytes = writer.bytes();
  if (bytes->size() > memrpc::kDefaultMaxRequestBytes) {
    HLOGW("ScanTask payload %{public}zu bytes exceeds %{public}u byte slot limit",
          bytes->size(), memrpc::kDefaultMaxRequestBytes);
  }
  return true;
}

bool DecodeScanTask(const uint8_t* bytes, std::size_t size, ScanTask* task) {
  if (task == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  return DecodeScanTaskInternal(&reader, task);
}

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!EncodeScanResultInternal(result, &writer)) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeScanResult(const uint8_t* bytes, std::size_t size, ScanResult* result) {
  if (result == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  return DecodeScanResultInternal(&reader, result);
}

bool EncodeBehaviorScanResult(const BehaviorScanResult& result, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!EncodeBehaviorScanResultInternal(result, &writer)) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeBehaviorScanResult(const uint8_t* bytes, std::size_t size, BehaviorScanResult* result) {
  if (result == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  return DecodeBehaviorScanResultInternal(&reader, result);
}

bool EncodeScanBehaviorRequest(const ScanBehaviorRequest& request, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  const bool ok = writer.WriteUint32(request.accessToken) && writer.WriteString(request.event) &&
                  writer.WriteString(request.bundleName);
  if (!ok) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeScanBehaviorRequest(const uint8_t* bytes,
                               std::size_t size,
                               ScanBehaviorRequest* request) {
  if (request == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  return reader.ReadUint32(&request->accessToken) && reader.ReadString(&request->event) &&
         reader.ReadString(&request->bundleName);
}

bool EncodeAccessTokenRequest(const AccessTokenRequest& request, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteUint32(request.accessToken)) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeAccessTokenRequest(const uint8_t* bytes, std::size_t size, AccessTokenRequest* request) {
  return request != nullptr && ByteReader(bytes, size).ReadUint32(&request->accessToken);
}

bool EncodeInt32Result(int32_t value, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteInt32(value)) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeInt32Result(const uint8_t* bytes, std::size_t size, int32_t* value) {
  return value != nullptr && ByteReader(bytes, size).ReadInt32(value);
}

bool EncodeBoolRequest(bool enabled, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!WriteBool(&writer, enabled)) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeBoolRequest(const uint8_t* bytes, std::size_t size, bool* enabled) {
  if (enabled == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  return ReadBool(&reader, enabled);
}

bool EncodePollBehaviorEventReply(const PollBehaviorEventReply& reply, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteInt32(reply.result) || !writer.WriteUint32(reply.accessToken)) {
    return false;
  }
  std::vector<uint8_t> event_bytes;
  if (!EncodeBehaviorScanResult(reply.scanResult, &event_bytes) ||
      !writer.WriteBytes(event_bytes.data(), static_cast<uint32_t>(event_bytes.size()))) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodePollBehaviorEventReply(const uint8_t* bytes,
                                  std::size_t size,
                                  PollBehaviorEventReply* reply) {
  if (reply == nullptr) {
    return false;
  }
  ByteReader reader(bytes, size);
  if (!reader.ReadInt32(&reply->result) || !reader.ReadUint32(&reply->accessToken)) {
    return false;
  }
  return DecodeBehaviorScanResultInternal(&reader, &reply->scanResult);
}

bool EncodeScanFileReply(int32_t resultCode,
                         const ScanResult& scanResult,
                         std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteInt32(resultCode) || !EncodeScanResultInternal(scanResult, &writer)) {
    return false;
  }
  *bytes = writer.bytes();
  if (bytes->size() > memrpc::kDefaultMaxResponseBytes) {
    HLOGW("ScanFileReply payload %{public}zu bytes exceeds %{public}u byte slot limit",
          bytes->size(), memrpc::kDefaultMaxResponseBytes);
  }
  return true;
}

bool DecodeScanFileReply(const uint8_t* bytes,
                         std::size_t size,
                         int32_t* resultCode,
                         ScanResult* scanResult) {
  if (resultCode == nullptr || scanResult == nullptr || size < sizeof(int32_t)) {
    return false;
  }
  ByteReader reader(bytes, size);
  if (!reader.ReadInt32(resultCode)) {
    return false;
  }
  return DecodeScanResultInternal(&reader, scanResult);
}

}  // namespace OHOS::Security::VirusProtectionService
