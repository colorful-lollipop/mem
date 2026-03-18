#include "ves/ves_engine_service.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "ves/ves_session_service.h"
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

void VesEngineService::SetEventPublisher(std::weak_ptr<VesEventPublisher> publisher) {
    std::lock_guard<std::mutex> lock(eventPublisherMutex_);
    eventPublisher_ = std::move(publisher);
}

ScanFileReply VesEngineService::ScanFile(const ScanTask& request) const {
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

    return result;
}

MemRpc::StatusCode VesEngineService::PublishEvent(uint32_t eventType,
                                                  const std::vector<uint8_t>& payload,
                                                  uint32_t flags,
                                                  uint32_t eventDomain) const {
    std::shared_ptr<VesEventPublisher> publisher;
    {
        std::lock_guard<std::mutex> lock(eventPublisherMutex_);
        publisher = eventPublisher_.lock();
    }
    if (publisher == nullptr) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    MemRpc::RpcEvent event;
    event.eventDomain = eventDomain;
    event.eventType = eventType;
    event.flags = flags;
    event.payload = payload;
    return publisher->PublishEventBlocking(event);
}

MemRpc::StatusCode VesEngineService::PublishTextEvent(uint32_t eventType,
                                                      const std::string& payload,
                                                      uint32_t flags,
                                                      uint32_t eventDomain) const {
    return PublishEvent(eventType,
                        std::vector<uint8_t>(payload.begin(), payload.end()),
                        flags,
                        eventDomain);
}

void VesEngineService::RegisterHandlers(RpcHandlerSink* sink) {
    RegisterTypedHandler<ScanTask, ScanFileReply>(
        sink,
        static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
        [this](const ScanTask& request) { return ScanFile(request); });
}

}  // namespace VirusExecutorService
