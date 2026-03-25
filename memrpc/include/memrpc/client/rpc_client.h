#ifndef MEMRPC_CLIENT_RPC_CLIENT_H_
#define MEMRPC_CLIENT_RPC_CLIENT_H_

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "memrpc/core/bootstrap.h"
#include "memrpc/core/protocol.h"
#include "memrpc/core/types.h"

namespace MemRpc {

using RpcThenExecutor = std::function<void(std::function<void()>)>;

struct RpcCall {
    Opcode opcode = OPCODE_INVALID;
    Priority priority = Priority::Normal;
    // admission_timeout_ms 作用在 client 等待 request ring 可写阶段。
    // 0 = 无限等待。
    uint32_t admissionTimeoutMs = 0;
    // wait_for_recovery 让框架在 cooldown / restart 恢复窗口内先内部等待，
    // 而不是立刻把 CooldownActive 返给调用方。
    bool waitForRecovery = false;
    // recovery_timeout_ms 仅在 wait_for_recovery=true 时生效。
    // 0 = 无限等待。
    uint32_t recoveryTimeoutMs = 0;
    // queue_timeout_ms 作用在服务端排队阶段。0 = 无限等待。
    uint32_t queueTimeoutMs = 0;
    // exec_timeout_ms 从 client 侧请求成功发布到 request ring 后开始计时，
    // 直到收到最终 reply 为止。超时后返回 ExecTimeout，但不会取消服务端执行；
    // 如果真实 reply 晚到，client 会直接忽略。
    uint32_t execTimeoutMs = 30000;
    std::vector<uint8_t> payload;
};

struct RpcReply {
    // status 是框架层结果；errorCode 由业务 handler 自行定义。
    StatusCode status = StatusCode::Ok;
    int32_t errorCode = 0;
    std::vector<uint8_t> payload;
};

struct RpcClientRuntimeStats {
    uint32_t queuedSubmissions = 0;
    uint32_t pendingCalls = 0;
    uint32_t highRequestRingPending = 0;
    uint32_t normalRequestRingPending = 0;
    uint32_t responseRingPending = 0;
    bool waitingForRequestCredit = false;
    bool recoveryPending = false;
    uint32_t cooldownRemainingMs = 0;
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
    [[nodiscard]] bool IsReady() const;
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

enum class FailureStage : uint8_t {
    Admission = 0,
    Response = 1,
    Session = 2,
    Timeout = 3,
};

struct RpcFailure {
    StatusCode status = StatusCode::Ok;
    Opcode opcode = OPCODE_INVALID;
    Priority priority = Priority::Normal;
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

enum class RecoveryAction {  // NOLINT(performance-enum-size)
    Ignore,
    Restart,
    IdleClose,
    // internal-only terminal transition used by explicit Shutdown()
    ManualShutdown,
};

struct RecoveryDecision {
    RecoveryAction action = RecoveryAction::Ignore;
    uint32_t delayMs = 0;
};

enum class ClientLifecycleState : uint8_t {
    Uninitialized = 0,
    Active = 1,
    Disconnected = 2,
    Cooldown = 3,
    IdleClosed = 4,
    Recovering = 5,
    Closed = 6,
};

enum class RecoveryTrigger : uint8_t {
    Unknown = 0,
    ManualShutdown = 1,
    ExecTimeout = 2,
    EngineDeath = 3,
    ExternalHealthSignal = 4,
    IdlePolicy = 5,
    DemandReconnect = 6,
};

struct RecoveryRuntimeSnapshot {
    ClientLifecycleState lifecycleState = ClientLifecycleState::Uninitialized;
    RecoveryTrigger lastTrigger = RecoveryTrigger::Unknown;
    RecoveryAction lastRecoveryAction = RecoveryAction::Ignore;
    bool recoveryPending = false;
    bool terminalManualShutdown = false;
    uint32_t cooldownRemainingMs = 0;
    uint64_t currentSessionId = 0;
    uint64_t lastOpenedSessionId = 0;
    uint64_t lastClosedSessionId = 0;
};

struct RecoveryEventReport {
    ClientLifecycleState previousState = ClientLifecycleState::Uninitialized;
    ClientLifecycleState state = ClientLifecycleState::Uninitialized;
    RecoveryTrigger trigger = RecoveryTrigger::Unknown;
    RecoveryAction action = RecoveryAction::Ignore;
    bool terminalManualShutdown = false;
    bool recoveryPending = false;
    uint32_t cooldownDelayMs = 0;
    uint32_t cooldownRemainingMs = 0;
    uint64_t sessionId = 0;
    uint64_t previousSessionId = 0;
    uint64_t monotonicMs = 0;
};

struct RecoveryPolicy {
    std::function<RecoveryDecision(const RpcFailure&)> onFailure;
    // onIdle 收到的是“自最后一次活动以来累计 idle 总时长”。
    // 框架内部按固定 watchdog 节拍在“无 pending / 无排队 / session live”时采样回调；
    // idle 阈值是否成立、是否需要 CloseSession / Restart，由业务回调自己判断。
    std::function<RecoveryDecision(uint64_t idle_ms)> onIdle;
    std::function<RecoveryDecision(const EngineDeathReport&)> onEngineDeath;
};

enum class SessionOpenReason : uint8_t {
    InitialInit = 0,
    RestartRecovery = 1,
    ExternalRecovery = 2,
    DemandReconnect = 3,
};

struct SessionReadyReport {
    uint64_t sessionId = 0;
    uint64_t previousSessionId = 0;
    uint32_t generation = 0;
    uint32_t scheduledDelayMs = 0;
    uint64_t monotonicMs = 0;
    SessionOpenReason reason = SessionOpenReason::InitialInit;
};

using SessionReadyCallback = std::function<void(const SessionReadyReport&)>;
using RecoveryEventCallback = std::function<void(const RecoveryEventReport&)>;

enum class ExternalRecoverySignal : uint8_t {
    ChannelHealthTimeout = 0,
    ChannelHealthMalformed = 1,
    ChannelHealthUnhealthy = 2,
    ChannelHealthSessionMismatch = 3,
};

struct ExternalRecoveryRequest {
    ExternalRecoverySignal signal = ExternalRecoverySignal::ChannelHealthTimeout;
    uint64_t sessionId = 0;
    uint32_t delayMs = 0;
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
    void SetSessionReadyCallback(SessionReadyCallback callback);
    void SetRecoveryEventCallback(RecoveryEventCallback callback);
    void SetRecoveryPolicy(RecoveryPolicy policy);
    void RequestExternalRecovery(ExternalRecoveryRequest request);
    // Init 负责建立 session、映射共享内存并启动响应分发线程。
    StatusCode Init();
    // InvokeAsync 是框架层一等接口；失败时返回 ready future。
    RpcFuture InvokeAsync(const RpcCall& call);
    RpcFuture InvokeAsync(RpcCall&& call);
    // InvokeWithRecovery 只在 client 处于内部恢复窗口时等待并重试 invoke。
    // 它不会透明重放已经成功发布到旧 session 的 pending 请求；仅用于对
    // CooldownActive / PeerDisconnected 这类“尚未稳定进入可用 session”的调用做包装。
    StatusCode InvokeWithRecovery(const std::function<StatusCode()>& invoke,
                                  uint32_t minRecoveryWaitMs = 0,
                                  uint32_t retryGraceMs = 0);
    [[nodiscard]] RpcClientRuntimeStats GetRuntimeStats() const;
    [[nodiscard]] RecoveryRuntimeSnapshot GetRecoveryRuntimeSnapshot() const;
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CLIENT_RPC_CLIENT_H_
