#include "apps/minirpc/parent/minirpc_resilient_invoker.h"

#include <utility>

namespace OHOS::Security::VirusProtectionService::MiniRpc {

ResilientBatchInvoker::ResilientBatchInvoker(
    std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap,
    ReplayPolicy policy)
    : client_(std::move(bootstrap)), policy_(std::move(policy)) {}

MemRpc::StatusCode ResilientBatchInvoker::Init() {
    return client_.Init();
}

std::vector<ResilientBatchInvoker::TrackedFuture> ResilientBatchInvoker::SubmitBatch(
    const std::vector<MemRpc::RpcCall>& calls) {
    std::vector<TrackedFuture> tracked;
    tracked.reserve(calls.size());
    for (const auto& call : calls) {
        ActiveCall ac;
        ac.sequence_id = next_sequence_id_++;
        ac.original_call = call;
        ac.future = client_.InvokeAsync(call);

        TrackedFuture tf;
        tf.sequence_id = ac.sequence_id;
        tf.future = ac.future;
        tracked.push_back(std::move(tf));

        active_calls_.push_back(std::move(ac));
    }
    return tracked;
}

void ResilientBatchInvoker::CollectResults(std::vector<MemRpc::RpcReply>* completed_replies) {
    if (completed_replies == nullptr) {
        return;
    }

    for (auto& ac : active_calls_) {
        MemRpc::RpcReply reply;
        auto status = ac.future.WaitAndTake(&reply);
        if (status == MemRpc::StatusCode::Ok) {
            completed_replies->push_back(std::move(reply));
        } else {
            FailedCallRecord record;
            record.sequence_id = ac.sequence_id;
            record.opcode = ac.original_call.opcode;
            record.payload = ac.original_call.payload;
            record.failure_status = status;
            record.failed_at = std::chrono::steady_clock::now();
            failed_calls_.push_back(std::move(record));
        }
    }
    active_calls_.clear();
}

const std::vector<FailedCallRecord>& ResilientBatchInvoker::GetFailedCalls() const {
    return failed_calls_;
}

std::vector<ResilientBatchInvoker::TrackedFuture> ResilientBatchInvoker::ReplayFailed() {
    std::vector<TrackedFuture> replayed;
    std::vector<FailedCallRecord> skipped;

    for (const auto& record : failed_calls_) {
        ReplayDecision decision = ReplayDecision::Replay;
        if (policy_) {
            decision = policy_(record);
        }
        if (decision == ReplayDecision::Skip) {
            skipped.push_back(record);
            continue;
        }
        MemRpc::RpcCall call;
        call.opcode = record.opcode;
        call.payload = record.payload;

        ActiveCall ac;
        ac.sequence_id = next_sequence_id_++;
        ac.original_call = call;
        ac.future = client_.InvokeAsync(call);

        TrackedFuture tf;
        tf.sequence_id = ac.sequence_id;
        tf.future = ac.future;
        replayed.push_back(std::move(tf));

        active_calls_.push_back(std::move(ac));
    }
    failed_calls_ = std::move(skipped);
    return replayed;
}

void ResilientBatchInvoker::ClearFailedCalls() {
    failed_calls_.clear();
}

void ResilientBatchInvoker::SetReplayPolicy(ReplayPolicy policy) {
    policy_ = std::move(policy);
}

void ResilientBatchInvoker::Shutdown() {
    client_.Shutdown();
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
