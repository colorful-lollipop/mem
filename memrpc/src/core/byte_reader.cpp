#include "memrpc/core/byte_reader.h"

#include <cstring>

#include "virus_protection_service_log.h"

namespace MemRpc {

ByteReader::ByteReader(const std::vector<uint8_t>& bytes)
    : bytes_(bytes.data()),
      size_(bytes.size())
{
}

ByteReader::ByteReader(const uint8_t* bytes, std::size_t size)
    : bytes_(bytes),
      size_(size)
{
}

bool ByteReader::ReadUint32(uint32_t* value)
{
    return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadInt32(int32_t* value)
{
    return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadBytes(void* data, uint32_t size)
{
    if (data == nullptr) {
        HILOGE("ByteReader::ReadBytes failed: data is null size=%{public}u", size);
        return false;
    }
    if (bytes_ == nullptr) {
        HILOGE("ByteReader::ReadBytes failed: source is null offset=%{public}zu size=%{public}u", offset_, size);
        return false;
    }
    if (offset_ + size > size_) {
        HILOGE("ByteReader::ReadBytes failed: offset=%{public}zu size=%{public}u total=%{public}zu",
               offset_,
               size,
               size_);
        return false;
    }
    std::memcpy(data, bytes_ + offset_, size);
    offset_ += size;
    return true;
}

bool ByteReader::ReadBytesView(uint32_t size, ByteView* value)
{
    if (value == nullptr) {
        HILOGE("ByteReader::ReadBytesView failed: value is null size=%{public}u", size);
        return false;
    }
    if (bytes_ == nullptr) {
        HILOGE("ByteReader::ReadBytesView failed: source is null offset=%{public}zu size=%{public}u", offset_, size);
        return false;
    }
    if (offset_ + size > size_) {
        HILOGE("ByteReader::ReadBytesView failed: offset=%{public}zu size=%{public}u total=%{public}zu",
               offset_,
               size,
               size_);
        return false;
    }
    *value = ByteView(bytes_ + offset_, size);
    offset_ += size;
    return true;
}

bool ByteReader::ReadString(std::string* value)
{
    if (value == nullptr) {
        HILOGE("ByteReader::ReadString failed: value is null");
        return false;
    }
    uint32_t size = 0;
    if (!ReadUint32(&size)) {
        HILOGE("ByteReader::ReadString failed: size prefix missing");
        return false;
    }
    if (bytes_ == nullptr) {
        HILOGE("ByteReader::ReadString failed: source is null size=%{public}u", size);
        return false;
    }
    if (offset_ + size > size_) {
        HILOGE("ByteReader::ReadString failed: offset=%{public}zu size=%{public}u total=%{public}zu",
               offset_,
               size,
               size_);
        return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes_ + offset_), size);
    offset_ += size;
    return true;
}

bool ByteReader::ReadStringView(std::string_view* value)
{
    if (value == nullptr) {
        HILOGE("ByteReader::ReadStringView failed: value is null");
        return false;
    }
    uint32_t size = 0;
    if (!ReadUint32(&size)) {
        HILOGE("ByteReader::ReadStringView failed: size prefix missing");
        return false;
    }
    if (bytes_ == nullptr) {
        HILOGE("ByteReader::ReadStringView failed: source is null size=%{public}u", size);
        return false;
    }
    if (offset_ + size > size_) {
        HILOGE("ByteReader::ReadStringView failed: offset=%{public}zu size=%{public}u total=%{public}zu",
               offset_,
               size,
               size_);
        return false;
    }
    *value = std::string_view(reinterpret_cast<const char*>(bytes_ + offset_), size);
    offset_ += size;
    return true;
}

}  // namespace MemRpc
