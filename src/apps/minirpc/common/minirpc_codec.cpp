#include "apps/minirpc/common/minirpc_codec.h"

#include "core/byte_reader.h"
#include "core/byte_writer.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

using ::memrpc::ByteReader;
using ::memrpc::ByteWriter;

bool AssignBytes(const ByteWriter& writer, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

}  // namespace

bool EncodeEchoRequest(const EchoRequest& request, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteString(request.text) && AssignBytes(writer, bytes);
}

bool DecodeEchoRequest(const std::vector<uint8_t>& bytes, EchoRequest* request) {
  if (request == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadString(&request->text);
}

bool EncodeEchoReply(const EchoReply& reply, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteString(reply.text) && AssignBytes(writer, bytes);
}

bool DecodeEchoReply(const std::vector<uint8_t>& bytes, EchoReply* reply) {
  if (reply == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadString(&reply->text);
}

bool EncodeAddRequest(const AddRequest& request, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteInt32(request.lhs) && writer.WriteInt32(request.rhs) &&
         AssignBytes(writer, bytes);
}

bool DecodeAddRequest(const std::vector<uint8_t>& bytes, AddRequest* request) {
  if (request == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadInt32(&request->lhs) && reader.ReadInt32(&request->rhs);
}

bool EncodeAddReply(const AddReply& reply, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteInt32(reply.sum) && AssignBytes(writer, bytes);
}

bool DecodeAddReply(const std::vector<uint8_t>& bytes, AddReply* reply) {
  if (reply == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadInt32(&reply->sum);
}

bool EncodeSleepRequest(const SleepRequest& request, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteUint32(request.delay_ms) && AssignBytes(writer, bytes);
}

bool DecodeSleepRequest(const std::vector<uint8_t>& bytes, SleepRequest* request) {
  if (request == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadUint32(&request->delay_ms);
}

bool EncodeSleepReply(const SleepReply& reply, std::vector<uint8_t>* bytes) {
  ByteWriter writer;
  return writer.WriteInt32(reply.status) && AssignBytes(writer, bytes);
}

bool DecodeSleepReply(const std::vector<uint8_t>& bytes, SleepReply* reply) {
  if (reply == nullptr) {
    return false;
  }
  ByteReader reader(bytes);
  return reader.ReadInt32(&reply->status);
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
