#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_

#include <cstddef>
#include <vector>

#include "memrpc/core/codec.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_types.h"

namespace MemRpc {

template <>
struct CodecTraits<VirusExecutorService::InitReply> {
    static bool Encode(const VirusExecutorService::InitReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.code) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, VirusExecutorService::InitReply* reply) {
        if (reply == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code);
    }
};

template <>
struct CodecTraits<VirusExecutorService::ScanTask> {
    static bool Encode(const VirusExecutorService::ScanTask& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteString(request.path) && detail::AssignBytes(writer, bytes);
    }
    static bool Decode(const uint8_t* bytes, std::size_t size, VirusExecutorService::ScanTask* request) {
        if (request == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadString(&request->path);
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
        if (reply == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->code) && reader.ReadInt32(&reply->threatLevel);
    }
};

template <>
struct CodecTraits<VirusExecutorService::VesOpenSessionRequest> {
    static bool Encode(const VirusExecutorService::VesOpenSessionRequest& request,
                       std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        if (!writer.WriteUint32(request.version) ||
            !writer.WriteUint32(static_cast<uint32_t>(request.engineKinds.size()))) {
            return false;
        }
        for (uint32_t engineKind : request.engineKinds) {
            if (!writer.WriteUint32(engineKind)) {
                return false;
            }
        }
        return detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size,
                       VirusExecutorService::VesOpenSessionRequest* request) {
        if (request == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        request->engineKinds.clear();

        uint32_t engineCount = 0;
        if (!reader.ReadUint32(&request->version) || !reader.ReadUint32(&engineCount) ||
            engineCount > VirusExecutorService::VES_OPEN_SESSION_MAX_ENGINE_KINDS) {
            return false;
        }

        request->engineKinds.reserve(engineCount);
        for (uint32_t index = 0; index < engineCount; ++index) {
            uint32_t engineKind = 0;
            if (!reader.ReadUint32(&engineKind)) {
                return false;
            }
            request->engineKinds.push_back(engineKind);
        }
        return true;
    }
};

}  // namespace MemRpc

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_VES_VES_CODEC_H_
