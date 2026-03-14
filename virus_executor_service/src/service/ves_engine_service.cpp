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
    if (initialized_) {
        return;
    }
    initialized_ = true;
    HILOGI("VesEngineService initialized");
}

bool VesEngineService::initialized() const {
    return initialized_;
}

uint64_t VesEngineService::AddActiveTask(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    const uint64_t taskId = nextTaskId_++;
    activeTasks_.emplace(taskId, ActiveTask{MemRpc::MonotonicNowMs(), filePath});
    return taskId;
}

void VesEngineService::RemoveActiveTask(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    activeTasks_.erase(taskId);
}

ScanFileReply VesEngineService::ScanFile(const ScanFileRequest& request) {
    const uint64_t taskId = AddActiveTask(request.filePath);

    ScanFileReply result;
    if (!initialized_) {
        result.code = -1;
    } else {
        const auto behavior = EvaluateSamplePath(request.filePath);
        if (behavior.shouldCrash) {
            HILOGE("ScanFile(%{public}s): crash requested", request.filePath.c_str());
            std::abort();
        }
        if (behavior.sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(behavior.sleepMs));
        }
        result.code = 0;
        result.threatLevel = behavior.threatLevel;
    }
    HILOGI("ScanFile(%{public}s): threat=%{public}d",
          request.filePath.c_str(), result.threatLevel);
    RemoveActiveTask(taskId);

    return result;
}

VesHealthSnapshot VesEngineService::GetHealthSnapshot() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    VesHealthSnapshot snapshot;
    snapshot.inFlight = static_cast<uint32_t>(activeTasks_.size());
    if (activeTasks_.empty()) {
        snapshot.currentTask = "idle";
        return snapshot;
    }

    const uint32_t nowMs = MemRpc::MonotonicNowMs();
    const auto oldestTask = std::min_element(
        activeTasks_.begin(), activeTasks_.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second.startMonoMs < rhs.second.startMonoMs;
        });
    snapshot.currentTask = oldestTask->second.filePath;
    snapshot.lastTaskAgeMs = nowMs - oldestTask->second.startMonoMs;
    return snapshot;
}

void VesEngineService::RegisterHandlers(MemRpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    MemRpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        server, static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [this](const ScanFileRequest& r) { return ScanFile(r); });
}

}  // namespace VirusExecutorService
