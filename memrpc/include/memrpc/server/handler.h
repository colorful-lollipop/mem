#ifndef MEMRPC_SERVER_HANDLER_H_
#define MEMRPC_SERVER_HANDLER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "memrpc/core/protocol.h"
#include "memrpc/core/types.h"

namespace memrpc {

class PayloadView {
 public:
  PayloadView() = default;
  PayloadView(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  const uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  const uint8_t& front() const { return data_[0]; }
  const uint8_t* begin() const { return data_; }
  const uint8_t* end() const { return data_ + size_; }

  // Compatibility aliases (PascalCase)
  const uint8_t* Data() const { return data_; }
  std::size_t Size() const { return size_; }
  bool Empty() const { return empty(); }
  const uint8_t& Rront() const { return front(); }
  const uint8_t* Begin() const { return begin(); }
  const uint8_t* End() const { return end(); }

  std::vector<uint8_t> ToVector() const { return std::vector<uint8_t>(Begin(), End()); }

  operator std::vector<uint8_t>() const { return ToVector(); }

 private:
  const uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

inline bool operator==(PayloadView lhs, const std::vector<uint8_t>& rhs) {
  return lhs.Size() == rhs.size() &&
         std::equal(lhs.Begin(), lhs.End(), rhs.begin(), rhs.end());
}

inline bool operator==(const std::vector<uint8_t>& lhs, PayloadView rhs) {
  return rhs == lhs;
}

struct RpcServerCall {
  Opcode opcode = OPCODE_INVALID;
  Priority priority = Priority::Normal;
  uint32_t queueTimeoutMs = 0;
  uint32_t execTimeoutMs = 0;
  uint32_t flags = 0;
  PayloadView payload;
};

struct RpcServerReply {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engineCode = 0;
  int32_t detailCode = 0;
  std::vector<uint8_t> payload;
};

using RpcHandler = std::function<void(const RpcServerCall&, RpcServerReply*)>;

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_SERVER_HANDLER_H_
