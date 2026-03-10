#include "core/byte_reader.h"

#include <cstring>

namespace memrpc {

ByteReader::ByteReader(const std::vector<uint8_t>& bytes) : bytes_(bytes.data()), size_(bytes.size()) {}

ByteReader::ByteReader(const uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) {}

bool ByteReader::ReadUint32(uint32_t* value) {
  return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadInt32(int32_t* value) {
  return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadBytes(void* data, uint32_t size) {
  if (data == nullptr || bytes_ == nullptr || offset_ + size > size_) {
    return false;
  }
  std::memcpy(data, bytes_ + offset_, size);
  offset_ += size;
  return true;
}

bool ByteReader::ReadBytesView(uint32_t size, ByteView* value) {
  if (value == nullptr || bytes_ == nullptr || offset_ + size > size_) {
    return false;
  }
  *value = ByteView(bytes_ + offset_, size);
  offset_ += size;
  return true;
}

bool ByteReader::ReadString(std::string* value) {
  if (value == nullptr) {
    return false;
  }
  uint32_t size = 0;
  if (!ReadUint32(&size) || bytes_ == nullptr || offset_ + size > size_) {
    return false;
  }
  value->assign(reinterpret_cast<const char*>(bytes_ + offset_), size);
  offset_ += size;
  return true;
}

bool ByteReader::ReadStringView(std::string_view* value) {
  if (value == nullptr) {
    return false;
  }
  uint32_t size = 0;
  if (!ReadUint32(&size) || bytes_ == nullptr || offset_ + size > size_) {
    return false;
  }
  *value = std::string_view(reinterpret_cast<const char*>(bytes_ + offset_), size);
  offset_ += size;
  return true;
}

}  // namespace memrpc
