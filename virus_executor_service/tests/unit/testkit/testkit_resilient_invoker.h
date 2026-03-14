#ifndef VIRUS_EXECUTOR_SERVICE_TESTKIT_RESILIENT_INVOKER_H_
#define VIRUS_EXECUTOR_SERVICE_TESTKIT_RESILIENT_INVOKER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "memrpc/client/rpc_client.h"

namespace VirusExecutorService::testkit {

struct FailedCallRecord {
    uint64_t sequenceId = 0;
    MemRpc::Opcode opcode = MemRpc::OPCODE_INVALID;
    std::vector<uint8_t> payload;
    MemRpc::StatusCode failureStatus = MemRpc::StatusCode::PeerDisconnected;
    std::chrono::steady_clock::time_point failedAt;
};

enum class ReplayDecision { Replay, Skip };
using ReplayPolicy = std::function<ReplayDecision(const FailedCallRecord&)>;

class ResilientBatchInvoker {
 public:
    explicit ResilientBatchInvoker(
        std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap,
        ReplayPolicy policy = nullptr);

    MemRpc::StatusCode Init();

    struct TrackedFuture {
        uint64_t sequenceId = 0;
        MemRpc::RpcFuture future;
    };

    std::vector<TrackedFuture> SubmitBatch(const std::vector<MemRpc::RpcCall>& calls);

    void CollectResults(std::vector<MemRpc::RpcReply>* completedReplies);

    [[nodiscard]] const std::vector<FailedCallRecord>& GetFailedCalls() const;

    std::vector<TrackedFuture> ReplayFailed();

    void ClearFailedCalls();
    void SetReplayPolicy(ReplayPolicy policy);
    void Shutdown();

 private:
    struct ActiveCall {
        uint64_t sequenceId = 0;
        MemRpc::RpcCall originalCall;
        MemRpc::RpcFuture future;
    };

    MemRpc::RpcClient client_;
    ReplayPolicy policy_;
    uint64_t nextSequenceId_ = 1;
    std::vector<ActiveCall> activeCalls_;
    std::vector<FailedCallRecord> failedCalls_;
};

}  // namespace VirusExecutorService::testkit

#endif  // VIRUS_EXECUTOR_SERVICE_TESTKIT_RESILIENT_INVOKER_H_
