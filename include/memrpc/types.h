#ifndef MEMRPC_TYPES_H_
#define MEMRPC_TYPES_H_

#include <cstdint>
#include <string>

namespace memrpc {

enum class Priority {
  Normal = 0,
  High = 1,
};

struct ScanOptions {
  Priority priority = Priority::Normal;
  uint32_t queue_timeout_ms = 1000;
  uint32_t exec_timeout_ms = 30000;
  uint32_t flags = 0;
};

struct ScanRequest {
  std::string file_path;
  ScanOptions options;
};

enum class ScanVerdict {
  Clean = 0,
  Infected = 1,
  Unknown = 2,
  Error = 3,
};

enum class StatusCode {
  Ok = 0,
  QueueFull,
  QueueTimeout,
  ExecTimeout,
  PeerDisconnected,
  ProtocolMismatch,
  EngineInternalError,
  InvalidArgument,
};

struct ScanResult {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::string message;
};

}  // namespace memrpc

#endif  // MEMRPC_TYPES_H_
