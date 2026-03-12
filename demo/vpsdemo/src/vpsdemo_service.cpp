#include "vpsdemo_service.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "memrpc/server/typed_handler.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "vpsdemo_sample_rules.h"
#include "vpsdemo_types.h"
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

void VpsDemoService::Initialize() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    HLOGI("VpsDemoService initialized");
}

bool VpsDemoService::initialized() const {
    return initialized_;
}

ScanFileReply VpsDemoService::ScanFile(const ScanFileRequest& request) {
    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        in_flight_++;
        current_task_ = request.file_path;
        last_task_start_mono_ms_ = MonotonicNowMs();
    }

    ScanFileReply result;
    if (!initialized_) {
        result.code = -1;
    } else {
        const auto behavior = EvaluateSamplePath(request.file_path);
        if (behavior.shouldCrash) {
            HLOGE("ScanFile(%{public}s): crash requested", request.file_path.c_str());
            std::abort();
        }
        if (behavior.sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(behavior.sleepMs));
        }
        result.code = 0;
        result.threat_level = behavior.threatLevel;
    }
    HLOGI("ScanFile(%{public}s): threat=%{public}d",
          request.file_path.c_str(), result.threat_level);

    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        in_flight_--;
        if (in_flight_ == 0) {
            current_task_ = "idle";
        }
    }

    return result;
}

VpsHealthSnapshot VpsDemoService::GetHealthSnapshot() const {
    std::lock_guard<std::mutex> lock(health_mutex_);
    VpsHealthSnapshot snapshot;
    snapshot.in_flight = in_flight_;
    snapshot.current_task = current_task_;
    if (in_flight_ > 0 && last_task_start_mono_ms_ > 0) {
        snapshot.last_task_age_ms = MonotonicNowMs() - last_task_start_mono_ms_;
    }
    return snapshot;
}

void VpsDemoService::RegisterHandlers(memrpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    memrpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        server, static_cast<memrpc::Opcode>(DemoOpcode::DemoScanFile),
        [this](const ScanFileRequest& r) { return ScanFile(r); });
}

}  // namespace vpsdemo
