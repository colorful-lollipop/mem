#ifndef MEMRPC_SERVER_HANDLER_H_
#define MEMRPC_SERVER_HANDLER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "core/protocol.h"
#include "memrpc/core/types.h"

namespace memrpc {

struct RpcServerCall {
  Opcode opcode = Opcode::ScanFile;
  Priority priority = Priority::Normal;
  uint32_t queue_timeout_ms = 0;
  uint32_t exec_timeout_ms = 0;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
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
