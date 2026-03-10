#ifndef APPS_MINIRPC_PARENT_MINIRPC_RESILIENT_INVOKER_H_
#define APPS_MINIRPC_PARENT_MINIRPC_RESILIENT_INVOKER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "memrpc/client/rpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

struct FailedCallRecord {
    uint64_t sequence_id = 0;
    MemRpc::Opcode opcode = MemRpc::Opcode::ScanFile;
    std::vector<uint8_t> payload;
    MemRpc::StatusCode failure_status = MemRpc::StatusCode::PeerDisconnected;
    std::chrono::steady_clock::time_point failed_at;
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
        uint64_t sequence_id = 0;
        MemRpc::RpcFuture future;
    };

    std::vector<TrackedFuture> SubmitBatch(const std::vector<MemRpc::RpcCall>& calls);

    void CollectResults(std::vector<MemRpc::RpcReply>* completed_replies);

    const std::vector<FailedCallRecord>& GetFailedCalls() const;

    std::vector<TrackedFuture> ReplayFailed();

    void ClearFailedCalls();
    void SetReplayPolicy(ReplayPolicy policy);
    void Shutdown();

 private:
    struct ActiveCall {
        uint64_t sequence_id = 0;
        MemRpc::RpcCall original_call;
        MemRpc::RpcFuture future;
    };

    MemRpc::RpcClient client_;
    ReplayPolicy policy_;
    uint64_t next_sequence_id_ = 1;
    std::vector<ActiveCall> active_calls_;
    std::vector<FailedCallRecord> failed_calls_;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PARENT_MINIRPC_RESILIENT_INVOKER_H_
