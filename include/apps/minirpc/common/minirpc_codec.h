  #ifndef APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_
#define APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_

#include <vector>

#include "apps/minirpc/common/minirpc_types.h"
#include "core/byte_reader.h"
#include "core/byte_writer.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

namespace detail {

inline bool AssignBytes(const ::memrpc::ByteWriter& writer, std::vector<uint8_t>* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  *bytes = writer.bytes();
  return true;
}

}  // namespace detail

template <typename T>
struct CodecTraits;

template <typename T>
bool EncodeMessage(const T& value, std::vector<uint8_t>* bytes) {
  return CodecTraits<T>::Encode(value, bytes);
}

template <typename T>
bool DecodeMessage(const std::vector<uint8_t>& bytes, T* value) {
  return CodecTraits<T>::Decode(bytes, value);
}

template <>
struct CodecTraits<EchoRequest> {
  static bool Encode(const EchoRequest& request, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteString(request.text) && detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, EchoRequest* request)
  {
    if (request == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadString(&request->text);
  }
};

template <>
struct CodecTraits<EchoReply> {
  static bool Encode(const EchoReply& reply, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteString(reply.text) && detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, EchoReply* reply)
  {
    if (reply == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadString(&reply->text);
  }
};

template <>
struct CodecTraits<AddRequest> {
  static bool Encode(const AddRequest& request, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteInt32(request.lhs) && writer.WriteInt32(request.rhs) &&
           detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, AddRequest* request)
  {
    if (request == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadInt32(&request->lhs) && reader.ReadInt32(&request->rhs);
  }
};

template <>
struct CodecTraits<AddReply> {
  static bool Encode(const AddReply& reply, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteInt32(reply.sum) && detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, AddReply* reply)
  {
    if (reply == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadInt32(&reply->sum);
  }
};

template <>
struct CodecTraits<SleepRequest> {
  static bool Encode(const SleepRequest& request, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteUint32(request.delay_ms) && detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, SleepRequest* request)
  {
    if (request == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadUint32(&request->delay_ms);
  }
};

template <>
struct CodecTraits<SleepReply> {
  static bool Encode(const SleepReply& reply, std::vector<uint8_t>* bytes)
  {
    ::memrpc::ByteWriter writer;
    return writer.WriteInt32(reply.status) && detail::AssignBytes(writer, bytes);
  }

  static bool Decode(const std::vector<uint8_t>& bytes, SleepReply* reply)
  {
    if (reply == nullptr) {
      return false;
    }
    ::memrpc::ByteReader reader(bytes);
    return reader.ReadInt32(&reply->status);
  }
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_COMMON_MINIRPC_CODEC_H_
