#include "rpc/scan_codec.h"

#include <string>
#include <vector>

#include "core/byte_reader.h"
#include "core/byte_writer.h"
#include "core/protocol.h"

namespace memrpc {

bool EncodeScanRequest(const ScanRequest& request, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  ByteWriter writer;
  if (!writer.WriteString(request.file_path)) {
    return false;
  }
  if (writer.bytes().size() > kDefaultMaxRequestBytes) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

bool DecodeScanRequest(const std::vector<uint8_t>& bytes, ScanRequest* request) {
  if (request == nullptr || bytes.size() > kDefaultMaxRequestBytes) {
    return false;
  }
  ByteReader reader(bytes);
  std::string file_path;
  if (!reader.ReadString(&file_path)) {
    return false;
  }
  request->file_path = std::move(file_path);
  return true;
}

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes) {
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

bool DecodeScanResult(const std::vector<uint8_t>& bytes, ScanResult* result) {
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
