#include "vpsdemo/ves/ves_engine_service.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "memrpc/server/typed_handler.h"
#include "vpsdemo/ves/ves_codec.h"
#include "vpsdemo/ves/ves_protocol.h"
#include "vpsdemo/ves/vesdemo_sample_rules.h"
#include "vpsdemo/ves/ves_types.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

namespace {
uint32_t MonotonicNowMs() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}
}  // namespace

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

ScanFileReply VesEngineService::ScanFile(const ScanFileRequest& request) {
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        inFlight_++;
        currentTask_ = request.filePath;
        lastTaskStartMonoMs_ = MonotonicNowMs();
    }

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

    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        inFlight_--;
        if (inFlight_ == 0) {
            currentTask_ = "idle";
        }
    }

    return result;
}

VesHealthSnapshot VesEngineService::GetHealthSnapshot() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    VesHealthSnapshot snapshot;
    snapshot.inFlight = inFlight_;
    snapshot.currentTask = currentTask_;
    if (inFlight_ > 0 && lastTaskStartMonoMs_ > 0) {
        snapshot.lastTaskAgeMs = MonotonicNowMs() - lastTaskStartMonoMs_;
    }
    return snapshot;
}

void VesEngineService::RegisterHandlers(memrpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    memrpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        server, static_cast<memrpc::Opcode>(VesOpcode::ScanFile),
        [this](const ScanFileRequest& r) { return ScanFile(r); });
}

}  // namespace vpsdemo
