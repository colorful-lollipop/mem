#ifndef MEMRPC_CORE_BYTE_WRITER_H_
#define MEMRPC_CORE_BYTE_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace memrpc {

class ByteWriter {
 public:
  ByteWriter() = default;

  bool WriteUint32(uint32_t value);
  bool WriteInt32(int32_t value);
  bool WriteBytes(const void* data, uint32_t size);
  bool WriteString(const std::string& value);

  const std::vector<uint8_t>& bytes() const;

 private:
  std::vector<uint8_t> bytes_;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_BYTE_WRITER_H_
