#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_

#include <cstddef>
#include <vector>

#include "memrpc/core/codec.h"
#include "ves/ves_types.h"

namespace MemRpc {

template <>
struct CodecTraits<VirusExecutorService::InitReply> {
    static bool Encode(const VirusExecutorService::InitReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.code) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, VirusExecutorService::InitReply* reply) {
        if (reply == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code);
    }
};

template <>
struct CodecTraits<VirusExecutorService::ScanFileRequest> {
    static bool Encode(const VirusExecutorService::ScanFileRequest& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteString(request.filePath) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, VirusExecutorService::ScanFileRequest* request) {
        if (request == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadString(&request->filePath);
    }
};

template <>
struct CodecTraits<VirusExecutorService::ScanFileReply> {
    static bool Encode(const VirusExecutorService::ScanFileReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.code) && writer.WriteInt32(reply.threatLevel) &&
               detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, VirusExecutorService::ScanFileReply* reply) {
        if (reply == nullptr) return false;
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code) && reader.ReadInt32(&reply->threatLevel);
    }
};

}  // namespace MemRpc

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_
