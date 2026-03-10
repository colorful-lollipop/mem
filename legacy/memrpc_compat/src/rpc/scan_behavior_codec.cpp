#include "rpc/scan_behavior_codec.h"

#include <string>
#include <vector>

#include "memrpc/core/byte_reader.h"
#include "memrpc/core/byte_writer.h"
#include "core/protocol.h"

namespace memrpc {

bool EncodeScanBehaviorRequest(const ScanBehaviorRequest& request, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteString(request.behavior_text)) {
    return false;
  }
  if (writer.bytes().size() > kDefaultMaxRequestBytes) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeScanBehaviorRequest(const std::vector<uint8_t>& bytes, ScanBehaviorRequest* request) {
  if (request == nullptr || bytes.size() > kDefaultMaxRequestBytes) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadString(&request->behavior_text);
}

bool EncodeScanBehaviorResult(const ScanBehaviorResult& result, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteUint32(static_cast<uint32_t>(result.status)) ||
      !writer.WriteUint32(static_cast<uint32_t>(result.verdict)) ||
      !writer.WriteInt32(result.engine_code) ||
      !writer.WriteInt32(result.detail_code) ||
      !writer.WriteString(result.message)) {
    return false;
  }
  if (writer.bytes().size() > kDefaultMaxResponseBytes) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeScanBehaviorResult(const std::vector<uint8_t>& bytes, ScanBehaviorResult* result) {
  if (result == nullptr || bytes.size() > kDefaultMaxResponseBytes) {
    return false;
  }
  ByteReader reader(bytes);
  uint32_t status = 0;
  uint32_t verdict = 0;
  std::string message;
  if (!reader.ReadUint32(&status) || !reader.ReadUint32(&verdict) ||
      !reader.ReadInt32(&result->engine_code) || !reader.ReadInt32(&result->detail_code) ||
      !reader.ReadString(&message)) {
    return false;
  }
  result->status = static_cast<StatusCode>(status);
  result->verdict = static_cast<ScanVerdict>(verdict);
  result->message = std::move(message);
  return true;
}

}  // namespace memrpc
