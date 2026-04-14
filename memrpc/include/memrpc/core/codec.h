#ifndef MEMRPC_CORE_CODEC_H_
#define MEMRPC_CORE_CODEC_H_

#include <cstddef>
#include <string>
#include <vector>

#include "memrpc/core/byte_reader.h"
#include "memrpc/core/byte_writer.h"

namespace MemRpc {

namespace detail {

inline bool AssignBytes(const ByteWriter& writer, std::vector<uint8_t>* bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    *bytes = writer.bytes();
    return true;
}

inline bool AssignBytes(const ByteWriter& writer, std::string* bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    const auto& source = writer.bytes();
    if (source.empty()) {
        bytes->clear();
        return true;
    }
    bytes->assign(reinterpret_cast<const char*>(source.data()), source.size());
    return true;
}

inline const uint8_t* ByteData(const uint8_t* bytes)
{
    return bytes;
}

inline const uint8_t* ByteData(const char* bytes)
{
    return reinterpret_cast<const uint8_t*>(bytes);
}

}  // namespace detail

template <typename T>
struct CodecTraits;

template <typename T>
bool EncodeMessage(const T& value, std::vector<uint8_t>* bytes)
{
    return CodecTraits<T>::Encode(value, bytes);
}

template <typename T>
bool EncodeMessage(const T& value, std::string* bytes)
{
    std::vector<uint8_t> encoded;
    if (!CodecTraits<T>::Encode(value, &encoded)) {
        return false;
    }
    if (bytes == nullptr) {
        return false;
    }
    if (encoded.empty()) {
        bytes->clear();
        return true;
    }
    bytes->assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return true;
}

template <typename Message, typename BytesLike>
bool DecodeMessage(const BytesLike& bytes, Message* value)
{
    return CodecTraits<Message>::Decode(detail::ByteData(bytes.data()), bytes.size(), value);
}

template <typename T>
struct ViewTraits;

template <typename View, typename BytesLike>
bool DecodeMessageView(const BytesLike& bytes, View* value)
{
    return ViewTraits<View>::Decode(detail::ByteData(bytes.data()), bytes.size(), value);
}

}  // namespace MemRpc

#endif  // MEMRPC_CORE_CODEC_H_
