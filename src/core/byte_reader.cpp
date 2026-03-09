#include "core/byte_reader.h"

#include <cstring>

namespace memrpc {

ByteReader::ByteReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

bool ByteReader::ReadUint32(uint32_t* value) {
  return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadInt32(int32_t* value) {
  return ReadBytes(value, sizeof(*value));
}

bool ByteReader::ReadBytes(void* data, uint32_t size) {
  if (data == nullptr || offset_ + size > bytes_.size()) {
    return false;
  }
  std::memcpy(data, bytes_.data() + offset_, size);
  offset_ += size;
  return true;
}

bool ByteReader::ReadString(std::string* value) {
  if (value == nullptr) {
    return false;
  }
  uint32_t size = 0;
  if (!ReadUint32(&size) || offset_ + size > bytes_.size()) {
    return false;
  }
  value->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
  offset_ += size;
  return true;
}

}  // namespace memrpc
