#ifndef MEMRPC_CORE_BYTE_READER_H_
#define MEMRPC_CORE_BYTE_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace memrpc {

class ByteReader {
 public:
  explicit ByteReader(const std::vector<uint8_t>& bytes);
  ByteReader(const uint8_t* bytes, std::size_t size);

  bool ReadUint32(uint32_t* value);
  bool ReadInt32(int32_t* value);
  bool ReadBytes(void* data, uint32_t size);
  bool ReadString(std::string* value);

 private:
  const uint8_t* bytes_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_BYTE_READER_H_
