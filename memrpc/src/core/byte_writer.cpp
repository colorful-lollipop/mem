#include "memrpc/core/byte_writer.h"

#include <cstring>

#include "virus_protection_executor_log.h"

namespace MemRpc {

bool ByteWriter::WriteUint32(uint32_t value)
{
    return WriteBytes(&value, sizeof(value));
}

bool ByteWriter::WriteInt32(int32_t value)
{
    return WriteBytes(&value, sizeof(value));
}

bool ByteWriter::WriteBytes(const void* data, uint32_t size)
{
    if (data == nullptr && size != 0) {
        HILOGE("ByteWriter::WriteBytes failed: data is null size=%{public}u", size);
        return false;
    }
    const std::size_t offset = bytes_.size();
    bytes_.resize(offset + size);
    if (size != 0) {
        std::memcpy(bytes_.data() + offset, data, size);
    }
    return true;
}

bool ByteWriter::WriteString(const std::string& value)
{
    return WriteUint32(static_cast<uint32_t>(value.size())) &&
           WriteBytes(value.data(), static_cast<uint32_t>(value.size()));
}

const std::vector<uint8_t>& ByteWriter::bytes() const
{
    return bytes_;
}

}  // namespace MemRpc
