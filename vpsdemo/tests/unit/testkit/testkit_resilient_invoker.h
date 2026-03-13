#ifndef VPSDEMO_TESTKIT_RESILIENT_INVOKER_H_
#define VPSDEMO_TESTKIT_RESILIENT_INVOKER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "memrpc/client/rpc_client.h"

namespace vpsdemo::testkit {

struct FailedCallRecord {
    uint64_t sequenceId = 0;
    memrpc::Opcode opcode = memrpc::OPCODE_INVALID;
    std::vector<uint8_t> payload;
    memrpc::StatusCode failureStatus = memrpc::StatusCode::PeerDisconnected;
    std::chrono::steady_clock::time_point failedAt;
};

enum class ReplayDecision { Replay, Skip };
using ReplayPolicy = std::function<ReplayDecision(const FailedCallRecord&)>;

class ResilientBatchInvoker {
 public:
    explicit ResilientBatchInvoker(
        std::shared_ptr<memrpc::IBootstrapChannel> bootstrap,
        ReplayPolicy policy = nullptr);

    memrpc::StatusCode Init();

    struct TrackedFuture {
        uint64_t sequenceId = 0;
        memrpc::RpcFuture future;
    };

    std::vector<TrackedFuture> SubmitBatch(const std::vector<memrpc::RpcCall>& calls);

    void CollectResults(std::vector<memrpc::RpcReply>* completedReplies);

    const std::vector<FailedCallRecord>& GetFailedCalls() const;

    std::vector<TrackedFuture> ReplayFailed();

    void ClearFailedCalls();
    void SetReplayPolicy(ReplayPolicy policy);
    void Shutdown();

 private:
    struct ActiveCall {
        uint64_t sequenceId = 0;
        memrpc::RpcCall originalCall;
        memrpc::RpcFuture future;
    };

    memrpc::RpcClient client_;
    ReplayPolicy policy_;
    uint64_t nextSequenceId_ = 1;
    std::vector<ActiveCall> activeCalls_;
    std::vector<FailedCallRecord> failedCalls_;
};

}  // namespace vpsdemo::testkit

#endif  // VPSDEMO_TESTKIT_RESILIENT_INVOKER_H_
