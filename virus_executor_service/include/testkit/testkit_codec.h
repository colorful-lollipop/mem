#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CODEC_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CODEC_H_

#include <cstddef>
#include <vector>

#include "memrpc/core/codec.h"
#include "testkit/testkit_types.h"

namespace memrpc {

using virus_executor_service::testkit::AddReply;
using virus_executor_service::testkit::AddRequest;
using virus_executor_service::testkit::EchoReply;
using virus_executor_service::testkit::EchoRequest;
using virus_executor_service::testkit::SleepReply;
using virus_executor_service::testkit::SleepRequest;

template <>
struct CodecTraits<EchoRequest> {
    static bool Encode(const EchoRequest& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteString(request.text) && detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, EchoRequest* request) {
        if (request == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadString(&request->text);
    }
};

template <>
struct CodecTraits<EchoReply> {
    static bool Encode(const EchoReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteString(reply.text) && detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, EchoReply* reply) {
        if (reply == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadString(&reply->text);
    }
};

template <>
struct CodecTraits<AddRequest> {
    static bool Encode(const AddRequest& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(request.lhs) && writer.WriteInt32(request.rhs) &&
               detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, AddRequest* request) {
        if (request == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&request->lhs) && reader.ReadInt32(&request->rhs);
    }
};

template <>
struct CodecTraits<AddReply> {
    static bool Encode(const AddReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.sum) && detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, AddReply* reply) {
        if (reply == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->sum);
    }
};

template <>
struct CodecTraits<SleepRequest> {
    static bool Encode(const SleepRequest& request, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteUint32(request.delayMs) && detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, SleepRequest* request) {
        if (request == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadUint32(&request->delayMs);
    }
};

template <>
struct CodecTraits<SleepReply> {
    static bool Encode(const SleepReply& reply, std::vector<uint8_t>* bytes) {
        ByteWriter writer;
        return writer.WriteInt32(reply.status) && detail::AssignBytes(writer, bytes);
    }

    static bool Decode(const uint8_t* bytes, std::size_t size, SleepReply* reply) {
        if (reply == nullptr) {
            return false;
        }
        ByteReader reader(bytes, size);
        return reader.ReadInt32(&reply->status);
    }
};

}  // namespace memrpc

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TESTKIT_TESTKIT_CODEC_H_
