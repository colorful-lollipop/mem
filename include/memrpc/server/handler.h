#ifndef MEMRPC_SERVER_HANDLER_H_
#define MEMRPC_SERVER_HANDLER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/protocol.h"
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

  std::vector<uint8_t> ToVector() const { return std::vector<uint8_t>(begin(), end()); }

  operator std::vector<uint8_t>() const { return ToVector(); }

 private:
  const uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

inline bool operator==(PayloadView lhs, const std::vector<uint8_t>& rhs) {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

inline bool operator==(const std::vector<uint8_t>& lhs, PayloadView rhs) {
  return rhs == lhs;
}

struct RpcServerCall {
  Opcode opcode = Opcode::ScanFile;
  Priority priority = Priority::Normal;
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint32_t flags = 0;
  PayloadView payload;
};

struct RpcServerReply {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::vector<uint8_t> payload;
};

using RpcHandler = std::function<void(const RpcServerCall&, RpcServerReply*)>;

class IScanHandler {
 public:
  virtual ~IScanHandler() = default;

  virtual ScanResult HandleScan(const ScanRequest& request) = 0;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_SERVER_HANDLER_H_
