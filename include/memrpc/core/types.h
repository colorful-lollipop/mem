#ifndef MEMRPC_CORE_TYPES_H_
#define MEMRPC_CORE_TYPES_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace memrpc {

enum class Priority {
  Normal = 0,
  High = 1,
};

struct ScanOptions {
  // priority 决定请求进入高优或普通请求队列。
  Priority priority = Priority::Normal;
  // queue_timeout_ms 是请求在服务端排队阶段允许等待的最长时间。
  uint32_t queueTimeoutMs = 1000;
  // exec_timeout_ms 是 handler 实际执行阶段允许消耗的最长时间。
  uint32_t execTimeoutMs = 30000;
  uint32_t flags = 0;
};


struct ScanBehaviorRequest {
  std::string behavior_text;
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
  CrashedDuringExecution,
};

struct ScanResult {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engineCode = 0;
  int32_t detailCode = 0;
  std::string message;
};

struct ScanBehaviorResult {
  StatusCode status = StatusCode::Ok;
  ScanVerdict verdict = ScanVerdict::Unknown;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::string message;
};

struct RpcEvent {
  // event_domain/event_type 用于应用层自行划分事件命名空间。
  uint32_t event_domain = 0;
  uint32_t event_type = 0;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

using RpcEventCallback = std::function<void(const RpcEvent&)>;

enum class ReplayHint {
  Unknown = 0,
  SafeToReplay = 1,
  MaybeExecuted = 2,
};

enum class RpcRuntimeState {
  Unknown = 0,
  Free,
  Admitted,
  Queued,
  Executing,
  Responding,
  Ready,
  Consumed,
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CORE_TYPES_H_
