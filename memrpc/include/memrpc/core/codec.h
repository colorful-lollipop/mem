#ifndef MEMRPC_CORE_CODEC_H_
#define MEMRPC_CORE_CODEC_H_

#include <cstddef>
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

}  // namespace detail

template <typename T>
struct CodecTraits;

template <typename T>
bool EncodeMessage(const T& value, std::vector<uint8_t>* bytes)
{
    return CodecTraits<T>::Encode(value, bytes);
}

template <typename Message, typename BytesLike>
bool DecodeMessage(const BytesLike& bytes, Message* value)
{
    return CodecTraits<Message>::Decode(bytes.data(), bytes.size(), value);
}

template <typename T>
struct ViewTraits;

template <typename View, typename BytesLike>
bool DecodeMessageView(const BytesLike& bytes, View* value)
{
    return ViewTraits<View>::Decode(bytes.data(), bytes.size(), value);
}

}  // namespace MemRpc

#endif  // MEMRPC_CORE_CODEC_H_
