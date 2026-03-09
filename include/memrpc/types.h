#ifndef MEMRPC_TYPES_H_
#define MEMRPC_TYPES_H_

#include <cstdint>
#include <string>

namespace memrpc {

enum class Priority {
  kNormal = 0,
  kHigh = 1,
};

struct ScanOptions {
  Priority priority = Priority::kNormal;
  uint32_t queue_timeout_ms = 1000;
  uint32_t exec_timeout_ms = 30000;
  uint32_t flags = 0;
};

struct ScanRequest {
  std::string file_path;
  ScanOptions options;
};

enum class ScanVerdict {
  kClean = 0,
  kInfected = 1,
  kUnknown = 2,
  kError = 3,
};

enum class StatusCode {
  kOk = 0,
  kQueueFull,
  kQueueTimeout,
  kExecTimeout,
  kPeerDisconnected,
  kProtocolMismatch,
  kEngineInternalError,
  kInvalidArgument,
};

struct ScanResult {
  StatusCode status = StatusCode::kOk;
  ScanVerdict verdict = ScanVerdict::kUnknown;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::string message;
};

}  // namespace memrpc

#endif  // MEMRPC_TYPES_H_
