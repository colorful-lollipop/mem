#include "testkit_resilient_invoker.h"

#include <utility>

namespace VirusExecutorService::testkit {

namespace {

ReplayDecision DefaultReplayDecision(const FailedCallRecord& record)
{
    if (record.runtimeSnapshot.terminalManualShutdown ||
        record.runtimeSnapshot.lifecycleState == MemRpc::ClientLifecycleState::Closed) {
        return ReplayDecision::Skip;
    }
    if (record.hasRecoveryEvent && record.recoveryEvent.trigger == MemRpc::RecoveryTrigger::ManualShutdown) {
        return ReplayDecision::Skip;
    }
    if (record.hasRecoveryEvent && record.recoveryEvent.trigger == MemRpc::RecoveryTrigger::IdlePolicy) {
        return ReplayDecision::Skip;
    }
    if (record.replayHint == MemRpc::ReplayHint::SafeToReplay ||
        record.replayHint == MemRpc::ReplayHint::MaybeExecuted) {
        return ReplayDecision::Replay;
    }
    return ReplayDecision::Replay;
}

}  // namespace

ResilientBatchInvoker::ResilientBatchInvoker(std::shared_ptr<MemRpc::IBootstrapChannel> bootstrap, ReplayPolicy policy)
    : client_(std::move(bootstrap)),
      policy_(std::move(policy))
{
    client_.SetRecoveryEventCallback([this](const MemRpc::RecoveryEventReport& report) {
        lastRecoveryEvent_ = report;
        hasRecoveryEvent_ = true;
    });
}

MemRpc::StatusCode ResilientBatchInvoker::Init()
{
    return client_.Init();
}

std::vector<ResilientBatchInvoker::TrackedFuture> ResilientBatchInvoker::SubmitBatch(
    const std::vector<MemRpc::RpcCall>& calls)
{
    std::vector<TrackedFuture> tracked;
    tracked.reserve(calls.size());
    for (const auto& call : calls) {
        ActiveCall activeCall;
        activeCall.sequenceId = nextSequenceId_++;
        activeCall.originalCall = call;
        activeCall.future = client_.InvokeAsync(call);

        TrackedFuture trackedFuture;
        trackedFuture.sequenceId = activeCall.sequenceId;
        trackedFuture.future = activeCall.future;
        tracked.push_back(std::move(trackedFuture));

        activeCalls_.push_back(std::move(activeCall));
    }
    return tracked;
}

void ResilientBatchInvoker::CollectResults(std::vector<MemRpc::RpcReply>* completedReplies)
{
    if (completedReplies == nullptr) {
        return;
    }

    for (auto& activeCall : activeCalls_) {
        MemRpc::RpcReply reply;
        const auto status = activeCall.future.WaitAndTake(&reply);
        if (status == MemRpc::StatusCode::Ok) {
            completedReplies->push_back(std::move(reply));
        } else {
            FailedCallRecord record;
            record.sequenceId = activeCall.sequenceId;
            record.opcode = activeCall.originalCall.opcode;
            record.payload = activeCall.originalCall.payload;
            record.failureStatus = status;
            record.replayHint =
                status == MemRpc::StatusCode::QueueTimeout ? MemRpc::ReplayHint::SafeToReplay
                : status == MemRpc::StatusCode::ExecTimeout || status == MemRpc::StatusCode::CrashedDuringExecution
                    ? MemRpc::ReplayHint::MaybeExecuted
                    : MemRpc::ReplayHint::Unknown;
            record.runtimeSnapshot = client_.GetRecoveryRuntimeSnapshot();
            record.recoveryEvent = lastRecoveryEvent_;
            record.hasRecoveryEvent = hasRecoveryEvent_;
            record.failedAt = std::chrono::steady_clock::now();
            failedCalls_.push_back(std::move(record));
        }
    }
    activeCalls_.clear();
}

const std::vector<FailedCallRecord>& ResilientBatchInvoker::GetFailedCalls() const
{
    return failedCalls_;
}

std::vector<ResilientBatchInvoker::TrackedFuture> ResilientBatchInvoker::ReplayFailed()
{
    std::vector<TrackedFuture> replayed;
    std::vector<FailedCallRecord> skipped;

    for (const auto& record : failedCalls_) {
        ReplayDecision decision = policy_ ? policy_(record) : DefaultReplayDecision(record);
        if (decision == ReplayDecision::Skip) {
            skipped.push_back(record);
            continue;
        }

        MemRpc::RpcCall call;
        call.opcode = record.opcode;
        call.payload = record.payload;

        ActiveCall activeCall;
        activeCall.sequenceId = nextSequenceId_++;
        activeCall.originalCall = call;
        activeCall.future = client_.InvokeAsync(call);

        TrackedFuture trackedFuture;
        trackedFuture.sequenceId = activeCall.sequenceId;
        trackedFuture.future = activeCall.future;
        replayed.push_back(std::move(trackedFuture));

        activeCalls_.push_back(std::move(activeCall));
    }

    failedCalls_ = std::move(skipped);
    return replayed;
}

void ResilientBatchInvoker::ClearFailedCalls()
{
    failedCalls_.clear();
}

void ResilientBatchInvoker::SetReplayPolicy(ReplayPolicy policy)
{
    policy_ = std::move(policy);
}

void ResilientBatchInvoker::Shutdown()
{
    client_.Shutdown();
}

}  // namespace VirusExecutorService::testkit
