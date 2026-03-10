#include "memrpc/compat/scan_codec.h"

#include <vector>

#include "memrpc/core/byte_reader.h"
#include "memrpc/core/byte_writer.h"
#include "core/protocol.h"

namespace memrpc {

bool EncodeScanRequest(const ScanRequest& request, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr || request.file_path.size() > kDefaultMaxRequestBytes) {
    return false;
  }
  ByteWriter writer;
  return writer.WriteString(request.file_path) && ((*bytes = writer.bytes()), true);
}

bool DecodeScanRequest(const std::vector<uint8_t>& bytes, ScanRequest* request) {
  if (request == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadString(&request->file_path);
}

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr || result.message.size() > kDefaultMaxResponseBytes) {
    return false;
  }
  ByteWriter writer;
  return writer.WriteInt32(static_cast<int32_t>(result.status)) &&
         writer.WriteInt32(static_cast<int32_t>(result.verdict)) &&
         writer.WriteInt32(result.engine_code) && writer.WriteInt32(result.detail_code) &&
         writer.WriteString(result.message) && ((*bytes = writer.bytes()), true);
}

bool DecodeScanResult(const std::vector<uint8_t>& bytes, ScanResult* result) {
  if (result == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  int32_t status = 0;
  int32_t verdict = 0;
  return reader.ReadInt32(&status) && reader.ReadInt32(&verdict) &&
         reader.ReadInt32(&result->engine_code) && reader.ReadInt32(&result->detail_code) &&
         reader.ReadString(&result->message) &&
         ((result->status = static_cast<StatusCode>(status)),
          (result->verdict = static_cast<ScanVerdict>(verdict)), true);
}

}  // namespace memrpc
