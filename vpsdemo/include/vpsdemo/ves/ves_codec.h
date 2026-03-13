#ifndef INCLUDE_VPSDEMO_VES_VES_CODEC_H_
#define INCLUDE_VPSDEMO_VES_VES_CODEC_H_

#include <cstddef>
#include <vector>

#include "memrpc/core/codec.h"
#include "vpsdemo/ves/ves_types.h"

namespace memrpc {

template <>
struct CodecTraits<vpsdemo::InitReply> {
    static bool Encode(const vpsdemo::InitReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.code) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, vpsdemo::InitReply* reply) {
        if (reply == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code);
    }
};

template <>
struct CodecTraits<vpsdemo::ScanFileRequest> {
    static bool Encode(const vpsdemo::ScanFileRequest& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteString(request.filePath) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, vpsdemo::ScanFileRequest* request) {
        if (request == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadString(&request->filePath);
    }
};

template <>
struct CodecTraits<vpsdemo::ScanFileReply> {
    static bool Encode(const vpsdemo::ScanFileReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.code) && writer.WriteInt32(reply.threatLevel) &&
               detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, vpsdemo::ScanFileReply* reply) {
        if (reply == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code) && reader.ReadInt32(&reply->threatLevel);
    }
};

}  // namespace memrpc

#endif  // INCLUDE_VPSDEMO_VES_VES_CODEC_H_
