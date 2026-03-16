#include "ves/ves_engine_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "memrpc/core/runtime_utils.h"
#include "memrpc/server/typed_handler.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_sample_rules.h"
#include "ves/ves_types.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

void VesEngineService::Initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    HILOGI("VesEngineService initialized");
}

bool VesEngineService::initialized() const {
    return initialized_.load(std::memory_order_acquire);
}

uint64_t VesEngineService::AddActiveTask() {
    std::lock_guard<std::mutex> lock(healthMutex_);
    const uint64_t taskId = nextTaskId_++;
    activeTasks_.emplace(taskId, ActiveTask{MemRpc::MonotonicNowMs()});
    return taskId;
}

void VesEngineService::RemoveActiveTask(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    activeTasks_.erase(taskId);
}

ScanFileReply VesEngineService::ScanFile(const ScanTask& request) {
    const uint64_t taskId = AddActiveTask();

    ScanFileReply result;
    if (!initialized()) {
        result.code = -1;
    } else {
        const auto behavior = EvaluateSamplePath(request.path);
        if (behavior.shouldCrash) {
            HILOGE("ScanFile(%{public}s): crash requested", request.path.c_str());
            std::abort();
        }
        if (behavior.sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(behavior.sleepMs));
        }
        result.code = 0;
        result.threatLevel = behavior.threatLevel;
    }
    HILOGI("ScanFile(%{public}s): threat=%{public}d",
          request.path.c_str(), result.threatLevel);
    RemoveActiveTask(taskId);

    return result;
}

VesHealthSnapshot VesEngineService::GetHealthSnapshot() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    VesHealthSnapshot snapshot;
    if (!initialized()) {
        return snapshot;
    }

    snapshot.flags |= VES_HEARTBEAT_FLAG_INITIALIZED;
    snapshot.inFlight = static_cast<uint32_t>(activeTasks_.size());
    if (activeTasks_.empty()) {
        snapshot.currentTask = "idle";
        snapshot.status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
        snapshot.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::None);
        return snapshot;
    }

    const uint32_t nowMs = MemRpc::MonotonicNowMs();
    const auto oldestTask = std::min_element(
        activeTasks_.begin(), activeTasks_.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second.startMonoMs < rhs.second.startMonoMs;
        });
    snapshot.currentTask = "active";
    snapshot.lastTaskAgeMs = nowMs - oldestTask->second.startMonoMs;
    snapshot.flags |= VES_HEARTBEAT_FLAG_BUSY;
    if (snapshot.lastTaskAgeMs >= LONG_RUNNING_TASK_THRESHOLD_MS) {
        snapshot.status = static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning);
        snapshot.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::LongRunning);
        snapshot.flags |= VES_HEARTBEAT_FLAG_LONG_RUNNING;
        return snapshot;
    }
    snapshot.status = static_cast<uint32_t>(VesHeartbeatStatus::OkBusy);
    snapshot.reasonCode = static_cast<uint32_t>(VesHeartbeatReasonCode::Busy);
    return snapshot;
}

void VesEngineService::RegisterHandlers(MemRpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    MemRpc::RegisterTypedHandler<ScanTask, ScanFileReply>(
        server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [this](const ScanTask& r) { return ScanFile(r); });
}

}  // namespace VirusExecutorService
