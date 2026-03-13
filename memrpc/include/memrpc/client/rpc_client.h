#ifndef MEMRPC_CLIENT_RPC_CLIENT_H_
#define MEMRPC_CLIENT_RPC_CLIENT_H_

#include <chrono>
#include <memory>
#include <functional>
#include <vector>

#include "memrpc/core/protocol.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"

namespace MemRpc {

using RpcThenExecutor = std::function<void(std::function<void()>)>;

struct RpcCall {
  Opcode opcode = OPCODE_INVALID;
  Priority priority = Priority::Normal;
  // admission_timeout_ms 作用在 client 等待 slot / request ring 可写阶段。
  // 0 = 无限等待（slot 耗尽时阻塞直到可用）。
  uint32_t admissionTimeoutMs = 0;
  // queue_timeout_ms 作用在服务端排队阶段。0 = 无限等待。
  uint32_t queueTimeoutMs = 0;
  // exec_timeout_ms 作用在服务端 handler 执行阶段；当前为软超时，不强杀 handler。
  uint32_t execTimeoutMs = 30000;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

struct RpcReply {
  // status 是框架层结果；engine_code/detail_code 由业务 handler 自行定义。
  StatusCode status = StatusCode::Ok;
  int32_t engineCode = 0;
  int32_t detailCode = 0;
  std::vector<uint8_t> payload;
};

struct RpcClientRuntimeStats {
  uint32_t queuedSubmissions = 0;
  uint32_t pendingCalls = 0;
  uint32_t requestSlotCapacity = 0;
  uint32_t highRequestRingPending = 0;
  uint32_t normalRequestRingPending = 0;
  uint32_t responseRingPending = 0;
  bool waitingForRequestCredit = false;
};

class RpcFuture {
 public:
  RpcFuture();
  ~RpcFuture();

  RpcFuture(const RpcFuture&) = default;
  RpcFuture& operator=(const RpcFuture&) = default;
  RpcFuture(RpcFuture&&) noexcept = default;
  RpcFuture& operator=(RpcFuture&&) noexcept = default;

  // IsReady 只表示 reply 已经落地，不区分成功或失败。
  bool IsReady() const;
  // Wait 会阻塞到 reply 就绪，并返回 reply.status。
  StatusCode Wait(RpcReply* reply);
  // WaitAndTake 会阻塞到 reply 就绪，并把内部 reply move 给调用方。
  StatusCode WaitAndTake(RpcReply* reply);
  // WaitFor 在给定 deadline 内等待 reply；仅用于同步兼容路径。
  StatusCode WaitFor(RpcReply* reply, std::chrono::milliseconds timeout);
  // Then 注册完成回调，由 dispatcher 线程直接调用（或通过 executor 分发）。
  // 若 future 已 ready，回调在调用线程立即执行（或通过 executor 分发）。
  // 与 Wait/WaitFor/WaitAndTake 互斥——同一个 future 只能选一种消费方式。
  void Then(std::function<void(RpcReply)> callback, RpcThenExecutor executor = {});

 private:
  struct State;
  explicit RpcFuture(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend class RpcClient;
};

enum class FailureStage {
  Admission = 0,
  Response = 1,
  Session = 2,
  Timeout = 3,
};

struct RpcFailure {
  StatusCode status = StatusCode::Ok;
  Opcode opcode = OPCODE_INVALID;
  Priority priority = Priority::Normal;
  uint32_t flags = 0;
  uint32_t admissionTimeoutMs = 0;
  uint32_t queueTimeoutMs = 0;
  uint32_t execTimeoutMs = 0;
  uint64_t requestId = 0;
  uint64_t sessionId = 0;
  uint32_t monotonicMs = 0;
  FailureStage stage = FailureStage::Admission;
  ReplayHint replayHint = ReplayHint::Unknown;
  RpcRuntimeState lastRuntimeState = RpcRuntimeState::Unknown;
};

struct EngineDeathReport {
  uint64_t deadSessionId = 0;
  uint32_t safeToReplayCount = 0;

  struct PoisonPillSuspect {
    uint64_t requestId = 0;
    Opcode opcode = OPCODE_INVALID;
    RpcRuntimeState lastState = RpcRuntimeState::Unknown;
  };
  std::vector<PoisonPillSuspect> poisonPillSuspects;
};

enum class RecoveryAction { Ignore, Restart };

struct RecoveryDecision {
  RecoveryAction action = RecoveryAction::Ignore;
  uint32_t delayMs = 0;
};

struct RecoveryPolicy {
  std::function<RecoveryDecision(const RpcFailure&)> onFailure;
  std::function<RecoveryDecision(uint64_t idle_ms)> onIdle;
  std::function<RecoveryDecision(const EngineDeathReport&)> onEngineDeath;
  uint32_t idleTimeoutMs = 0;
  uint32_t idleNotifyIntervalMs = 0;
};

class RpcClient {
 public:
  explicit RpcClient(std::shared_ptr<IBootstrapChannel> bootstrap = nullptr);
  ~RpcClient();

  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;
  RpcClient(RpcClient&&) noexcept = default;
  RpcClient& operator=(RpcClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap);
  void SetEventCallback(RpcEventCallback callback);
  void SetRecoveryPolicy(RecoveryPolicy policy);
  // Init 负责建立 session、映射共享内存并启动响应分发线程。
  StatusCode Init();
  // InvokeAsync 是框架层一等接口；失败时返回 ready future。
  RpcFuture InvokeAsync(const RpcCall& call);
  RpcFuture InvokeAsync(RpcCall&& call);
  RpcClientRuntimeStats GetRuntimeStats() const;
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class RpcSyncClient {
 public:
  explicit RpcSyncClient(std::shared_ptr<IBootstrapChannel> bootstrap = nullptr);
  ~RpcSyncClient();

  RpcSyncClient(const RpcSyncClient&) = delete;
  RpcSyncClient& operator=(const RpcSyncClient&) = delete;
  RpcSyncClient(RpcSyncClient&&) noexcept = default;
  RpcSyncClient& operator=(RpcSyncClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap);
  void SetEventCallback(RpcEventCallback callback);
  void SetRecoveryPolicy(RecoveryPolicy policy);
  StatusCode Init();
  StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);
  RpcClientRuntimeStats GetRuntimeStats() const;
  void Shutdown();

 private:
  RpcClient client_;
};

}  // namespace MemRpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CLIENT_RPC_CLIENT_H_
